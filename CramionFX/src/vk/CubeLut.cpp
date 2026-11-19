#include "CramionFX/vk/CubeLut.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace cramion::gfx {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

// Siguiente palabra (separada por espacios o tabuladores).
std::string_view nextToken(std::string_view& s) {
    s = trim(s);
    std::size_t end = 0;
    while (end < s.size() && s[end] != ' ' && s[end] != '\t') ++end;
    const std::string_view token = s.substr(0, end);
    s.remove_prefix(end);
    return token;
}

bool parseFloat(std::string_view token, float& out) {
    // strtof sobre una copia terminada en cero (el from_chars de floats no
    // esta en todas las libc++ del NDK). Con la "C" de siempre: el punto
    // decimal no depende del idioma porque nadie llama a setlocale.
    if (token.empty() || token.size() >= 64) return false;
    char buffer[64];
    std::memcpy(buffer, token.data(), token.size());
    buffer[token.size()] = '\0';
    char* end = nullptr;
    out = std::strtof(buffer, &end);
    return end == buffer + token.size() && std::isfinite(out);
}

bool parseUint(std::string_view token, std::uint32_t& out) {
    if (token.empty()) return false;
    const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), out);
    return ec == std::errc() && ptr == token.data() + token.size();
}

bool parseThree(std::string_view rest, float (&v)[3]) {
    for (float& f : v) {
        if (!parseFloat(nextToken(rest), f)) return false;
    }
    return trim(rest).empty();
}

std::string lineError(std::size_t line, const std::string& what) {
    return "LUT .cube, linea " + std::to_string(line) + ": " + what;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// float -> half (IEEE 754 binario 16), redondeo al mas cercano.
std::uint16_t toHalf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFu;
    if (((bits >> 23) & 0xFFu) == 0xFFu) return static_cast<std::uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);  // infinito
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<std::uint16_t>(sign);  // cero
        mantissa |= 0x800000u;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - exponent);
        std::uint32_t half = mantissa >> shift;
        if ((mantissa >> (shift - 1)) & 1u) ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    if (mantissa & 0x1000u) ++half;  // redondeo (puede subir al exponente siguiente: correcto)
    return static_cast<std::uint16_t>(half);
}

}  // namespace

core::Vec3 CubeLut::at(std::uint32_t r, std::uint32_t g, std::uint32_t b) const {
    const std::size_t i = ((static_cast<std::size_t>(b) * size + g) * size + r) * 3;
    return core::Vec3{rgb[i], rgb[i + 1], rgb[i + 2]};
}

bool parseCubeLut(std::string_view text, CubeLut& out, std::string& error) {
    CubeLut lut;
    std::uint32_t size_3d = 0;
    std::uint32_t size_1d = 0;
    std::vector<float> values;
    std::size_t line_number = 0;
    std::size_t expected = 0;
    bool domain_set = false;
    // BOM de UTF-8.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        const char first = line.front();
        const bool numeric = (first >= '0' && first <= '9') || first == '-' || first == '+' || first == '.';
        if (numeric) {
            if (size_3d == 0 && size_1d == 0) {
                error = lineError(line_number, "datos antes de LUT_3D_SIZE o LUT_1D_SIZE");
                return false;
            }
            float v[3];
            if (!parseThree(line, v)) {
                error = lineError(line_number, "se esperaban tres numeros (r g b)");
                return false;
            }
            if (values.size() / 3 >= expected) {
                error = lineError(line_number, "sobran valores (mas de " + std::to_string(expected) + ")");
                return false;
            }
            values.insert(values.end(), {v[0], v[1], v[2]});
            continue;
        }
        std::string_view rest = line;
        const std::string_view key = nextToken(rest);
        if (key == "TITLE") {
            std::string_view t = trim(rest);
            if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
            lut.title = std::string(t);
        } else if (key == "LUT_3D_SIZE" || key == "LUT_1D_SIZE") {
            if (!values.empty() || size_3d != 0 || size_1d != 0) {
                error = lineError(line_number, "tamano repetido o despues de los datos");
                return false;
            }
            std::uint32_t n = 0;
            if (!parseUint(nextToken(rest), n) || !trim(rest).empty()) {
                error = lineError(line_number, "tamano no valido");
                return false;
            }
            if (key == "LUT_3D_SIZE") {
                if (n < 2 || n > kCubeLutMaxSize) {
                    error = lineError(line_number, "LUT_3D_SIZE debe ir de 2 a " + std::to_string(kCubeLutMaxSize));
                    return false;
                }
                size_3d = n;
                expected = static_cast<std::size_t>(n) * n * n;
            } else {
                if (n < 2 || n > 65536) {
                    error = lineError(line_number, "LUT_1D_SIZE debe ir de 2 a 65536");
                    return false;
                }
                size_1d = n;
                expected = n;
            }
            values.reserve(expected * 3);
        } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
            float v[3];
            if (!parseThree(rest, v)) {
                error = lineError(line_number, "se esperaban tres numeros en " + std::string(key));
                return false;
            }
            (key == "DOMAIN_MIN" ? lut.domain_min : lut.domain_max) = core::Vec3{v[0], v[1], v[2]};
            domain_set = true;
        } else if (key == "LUT_3D_INPUT_RANGE" || key == "LUT_1D_INPUT_RANGE") {
            float a = 0.0f;
            float b = 0.0f;
            if (!parseFloat(nextToken(rest), a) || !parseFloat(nextToken(rest), b) || !trim(rest).empty()) {
                error = lineError(line_number, "se esperaban dos numeros en " + std::string(key));
                return false;
            }
            if (!domain_set) {
                lut.domain_min = core::Vec3{a, a, a};
                lut.domain_max = core::Vec3{b, b, b};
            }
        } else {
            // Palabras clave que no cambian la tabla (p. ej. LUT_IN_VIDEO_RANGE
            // de Resolve): se ignoran, como hacen los demas lectores.
            continue;
        }
    }
    if (size_3d == 0 && size_1d == 0) {
        error = "LUT .cube: falta LUT_3D_SIZE o LUT_1D_SIZE";
        return false;
    }
    if (values.size() / 3 != expected) {
        error = "LUT .cube: se esperaban " + std::to_string(expected) + " valores y hay " + std::to_string(values.size() / 3);
        return false;
    }
    for (int c = 0; c < 3; ++c) {
        const float lo = c == 0 ? lut.domain_min.x : c == 1 ? lut.domain_min.y : lut.domain_min.z;
        const float hi = c == 0 ? lut.domain_max.x : c == 1 ? lut.domain_max.y : lut.domain_max.z;
        if (!(hi > lo)) {
            error = "LUT .cube: DOMAIN_MAX debe ser mayor que DOMAIN_MIN en cada canal";
            return false;
        }
    }
    if (size_3d != 0) {
        lut.size = size_3d;
        lut.rgb = std::move(values);
    } else {
        // 1D -> 3D: cada canal por su curva.
        const std::uint32_t n = kCubeLutFrom1DSize;
        lut.size = n;
        lut.rgb.resize(static_cast<std::size_t>(n) * n * n * 3);
        const auto curve = [&](int channel, float t) {
            const float x = t * static_cast<float>(size_1d - 1);
            const std::uint32_t i0 = std::min(static_cast<std::uint32_t>(x), size_1d - 1);
            const std::uint32_t i1 = std::min(i0 + 1, size_1d - 1);
            return lerp(values[i0 * 3 + channel], values[i1 * 3 + channel], x - static_cast<float>(i0));
        };
        for (std::uint32_t b = 0; b < n; ++b) {
            for (std::uint32_t g = 0; g < n; ++g) {
                for (std::uint32_t r = 0; r < n; ++r) {
                    const std::size_t i = ((static_cast<std::size_t>(b) * n + g) * n + r) * 3;
                    const float inv = 1.0f / static_cast<float>(n - 1);
                    lut.rgb[i] = curve(0, static_cast<float>(r) * inv);
                    lut.rgb[i + 1] = curve(1, static_cast<float>(g) * inv);
                    lut.rgb[i + 2] = curve(2, static_cast<float>(b) * inv);
                }
            }
        }
    }
    out = std::move(lut);
    return true;
}

bool loadCubeLut(const std::filesystem::path& file, CubeLut& out, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "LUT .cube: no se pudo abrir " + file.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    return parseCubeLut(text, out, error);
}

CubeLut makeIdentityCubeLut(std::uint32_t n) {
    CubeLut lut;
    n = std::clamp<std::uint32_t>(n, 2, kCubeLutMaxSize);
    lut.size = n;
    lut.title = "identidad";
    lut.rgb.resize(static_cast<std::size_t>(n) * n * n * 3);
    const float inv = 1.0f / static_cast<float>(n - 1);
    for (std::uint32_t b = 0; b < n; ++b) {
        for (std::uint32_t g = 0; g < n; ++g) {
            for (std::uint32_t r = 0; r < n; ++r) {
                const std::size_t i = ((static_cast<std::size_t>(b) * n + g) * n + r) * 3;
                lut.rgb[i] = static_cast<float>(r) * inv;
                lut.rgb[i + 1] = static_cast<float>(g) * inv;
                lut.rgb[i + 2] = static_cast<float>(b) * inv;
            }
        }
    }
    return lut;
}

core::Vec3 sampleCubeLut(const CubeLut& lut, const core::Vec3& color) {
    if (!lut.valid()) return color;
    const float n1 = static_cast<float>(lut.size - 1);
    const auto coord = [&](float v, float lo, float hi) {
        return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) * n1;
    };
    const float fr = coord(color.x, lut.domain_min.x, lut.domain_max.x);
    const float fg = coord(color.y, lut.domain_min.y, lut.domain_max.y);
    const float fb = coord(color.z, lut.domain_min.z, lut.domain_max.z);
    const std::uint32_t r0 = std::min(static_cast<std::uint32_t>(fr), lut.size - 2);
    const std::uint32_t g0 = std::min(static_cast<std::uint32_t>(fg), lut.size - 2);
    const std::uint32_t b0 = std::min(static_cast<std::uint32_t>(fb), lut.size - 2);
    const float tr = fr - static_cast<float>(r0);
    const float tg = fg - static_cast<float>(g0);
    const float tb = fb - static_cast<float>(b0);
    const auto mix3 = [](const core::Vec3& a, const core::Vec3& b, float t) {
        return core::Vec3{lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)};
    };
    const core::Vec3 c00 = mix3(lut.at(r0, g0, b0), lut.at(r0 + 1, g0, b0), tr);
    const core::Vec3 c10 = mix3(lut.at(r0, g0 + 1, b0), lut.at(r0 + 1, g0 + 1, b0), tr);
    const core::Vec3 c01 = mix3(lut.at(r0, g0, b0 + 1), lut.at(r0 + 1, g0, b0 + 1), tr);
    const core::Vec3 c11 = mix3(lut.at(r0, g0 + 1, b0 + 1), lut.at(r0 + 1, g0 + 1, b0 + 1), tr);
    return mix3(mix3(c00, c10, tg), mix3(c01, c11, tg), tb);
}

std::vector<std::uint16_t> cubeLutToHalfRgba(const CubeLut& lut) {
    std::vector<std::uint16_t> out;
    if (!lut.valid()) return out;
    const std::size_t texels = static_cast<std::size_t>(lut.size) * lut.size * lut.size;
    out.resize(texels * 4);
    const std::uint16_t one = toHalf(1.0f);
    for (std::size_t i = 0; i < texels; ++i) {
        out[i * 4 + 0] = toHalf(lut.rgb[i * 3 + 0]);
        out[i * 4 + 1] = toHalf(lut.rgb[i * 3 + 1]);
        out[i * 4 + 2] = toHalf(lut.rgb[i * 3 + 2]);
        out[i * 4 + 3] = one;
    }
    return out;
}

}  // namespace cramion::gfx
