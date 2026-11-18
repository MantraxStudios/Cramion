// Letra de trazos del texto de depuracion en el mundo (StrokeFont.h).

#include "CramionFX/vk/StrokeFont.h"

#include <cstdint>

namespace cramion::gfx {

namespace {

// Rejilla de 4 x 8 (x, y con y hacia arriba): cada polilinea es una lista de
// puntos "xy" seguidos; las polilineas se separan con un espacio.
struct Glyph {
    char c;
    const char* strokes;
};

constexpr Glyph kGlyphs[] = {
    {'0', "0040480800 0048"},
    {'1', "162820 1030"},
    {'2', "06182846450040"},
    {'3', "08484000 1444"},
    {'4', "30380242"},
    {'5', "480804444000"},
    {'6', "480800404404"},
    {'7', "084810"},
    {'8', "0040480800 0444"},
    {'9', "004048080444"},
    {'A', "0006284640 0444"},
    {'B', "00083847453404 3443413000"},
    {'C', "48080040"},
    {'D', "00082846422000"},
    {'E', "48080040 0434"},
    {'F', "480800 0434"},
    {'G', "480800404424"},
    {'H', "0008 4048 0444"},
    {'I', "1838 2820 1030"},
    {'J', "48400002"},
    {'K', "0008 480440"},
    {'L', "080040"},
    {'M', "0008244840"},
    {'N', "00084048"},
    {'O', "0040480800"},
    {'P', "0008484404"},
    {'Q', "0040480800 2240"},
    {'R', "0008484404 2440"},
    {'S', "480804444000"},
    {'T', "0848 2820"},
    {'U', "08004048"},
    {'V', "082048"},
    {'W', "0810243048"},
    {'X', "0048 0840"},
    {'Y', "082448 2420"},
    {'Z', "08480040"},
    {'.', "2021"},
    {',', "2110"},
    {':', "2223 2526"},
    {';', "2526 2310"},
    {'-', "0444"},
    {'+', "0444 2226"},
    {'=', "0343 0545"},
    {'_', "0040"},
    {'/', "0048"},
    {'\\', "0840"},
    {'(', "38161230"},
    {')', "18363210"},
    {'[', "38181030"},
    {']', "18383010"},
    {'{', "38161230"},
    {'}', "18363210"},
    {'<', "460442"},
    {'>', "064402"},
    {'|', "2028"},
    {'!', "2823 2021"},
    {'?', "060848452422 2021"},
    {'\'', "2826"},
    {'"', "1816 3836"},
    {'%', "0048 0807 4041"},
    {'#', "1018 3038 0343 0545"},
    {'*', "0444 2226 1335 1533"},
    {'$', "480804444000 2028"},
    {'^', "062846"},
    {'~', "05162534"},
    {'@', "4202 0008 0848 4824 2444"},
    {'&', "40081828170040"},
    {'`', "1827"},
    {'\x01', "1838372717"},  // grado (interno: el codigo de '\xB0')
};

constexpr int kAdvance = 6;      // unidades por letra (4 de letra + 2 de hueco)
constexpr int kLineHeight = 12;  // unidades por linea (8 de letra + 4 de hueco)

// Lee un caracter UTF-8 y devuelve el que se dibuja (ASCII) o 0 si no se sabe.
char nextChar(std::string_view text, std::size_t& i) {
    const auto b0 = static_cast<unsigned char>(text[i++]);
    if (b0 < 0x80) return static_cast<char>(b0);
    std::uint32_t cp = 0;
    int extra = 0;
    if ((b0 & 0xE0) == 0xC0) {
        cp = b0 & 0x1Fu;
        extra = 1;
    } else if ((b0 & 0xF0) == 0xE0) {
        cp = b0 & 0x0Fu;
        extra = 2;
    } else if ((b0 & 0xF8) == 0xF0) {
        cp = b0 & 0x07u;
        extra = 3;
    } else {
        return '?';
    }
    for (int k = 0; k < extra; ++k) {
        if (i >= text.size() || (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 0x3Fu);
    }
    switch (cp) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: return 'A';
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'E';
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: case 0xEC: case 0xED: case 0xEE: case 0xEF: return 'I';
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: return 'O';
        case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xF9: case 0xFA: case 0xFB: case 0xFC: return 'U';
        case 0xD1: case 0xF1: return 'N';
        case 0xC7: case 0xE7: return 'C';
        case 0xBF: return '?';
        case 0xA1: return '!';
        case 0xB0: return '\x01';
        default: return '?';
    }
}

}  // namespace

const char* strokeGlyph(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (const Glyph& g : kGlyphs) {
        if (g.c == c) return g.strokes;
    }
    return nullptr;
}

std::size_t strokeText(std::string_view text, float height, std::vector<StrokeSegment>& out, std::size_t max_chars) {
    if (text.empty() || !(height > 0.0f)) return 0;
    // Las lineas (en caracteres ya traducidos) para medirlas y centrarlas.
    std::vector<char> chars;
    chars.reserve(text.size());
    std::vector<std::size_t> line_start{0};
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size() && count < max_chars;) {
        const char c = nextChar(text, i);
        if (c == '\r') continue;
        if (c == '\n') {
            line_start.push_back(chars.size());
            continue;
        }
        chars.push_back(c == '\t' ? ' ' : c);
        ++count;
    }
    line_start.push_back(chars.size());
    const std::size_t line_count = line_start.size() - 1;
    const float unit = height / 8.0f;
    const float total_h = static_cast<float>(line_count * 8 + (line_count - 1) * (kLineHeight - 8));
    const std::size_t before = out.size();
    for (std::size_t l = 0; l < line_count; ++l) {
        const std::size_t first = line_start[l];
        const std::size_t last = line_start[l + 1];
        const std::size_t n = last - first;
        if (n == 0) continue;
        const float width = static_cast<float>(n * kAdvance - (kAdvance - 4));
        const float base_x = -width * 0.5f;
        const float base_y = total_h * 0.5f - 8.0f - static_cast<float>(l * kLineHeight);
        for (std::size_t k = 0; k < n; ++k) {
            const char* strokes = strokeGlyph(chars[first + k]);
            if (chars[first + k] == ' ') continue;
            if (strokes == nullptr) strokes = strokeGlyph('?');
            const float ox = base_x + static_cast<float>(k * kAdvance);
            // Cada polilinea: puntos de dos cifras seguidos.
            bool have_prev = false;
            float px = 0.0f, py = 0.0f;
            for (const char* s = strokes;; ++s) {
                if (*s == '\0' || *s == ' ') {
                    have_prev = false;
                    if (*s == '\0') break;
                    continue;
                }
                if (s[1] == '\0') break;  // punto a medias: mal escrito
                const float x = ox + static_cast<float>(s[0] - '0');
                const float y = base_y + static_cast<float>(s[1] - '0');
                ++s;
                if (have_prev) out.push_back(StrokeSegment{px * unit, py * unit, x * unit, y * unit});
                px = x;
                py = y;
                have_prev = true;
            }
        }
    }
    return out.size() - before;
}

}  // namespace cramion::gfx
