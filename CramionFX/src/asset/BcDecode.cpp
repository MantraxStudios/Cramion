// Descompresion por CPU de las texturas BC (BC1-BC5 y BC7) para las GPU que
// no las leen: casi todas las de movil (Mali, PowerVR; Adreno segun el
// driver). Sin esto, un modelo con texturas DDS no se podia crear en esos
// moviles. Ver Dds.h.
//
// Formatos (especificacion de Direct3D 11 / BPTC de OpenGL):
//   BC1  color 5:6:5 + 2 bits por texel (con alfa de 1 bit si c0 <= c1)
//   BC2  alfa explicito de 4 bits + bloque de color BC1 (siempre 4 colores)
//   BC3  bloque de alfa de 8 valores + bloque de color BC1 (siempre 4 colores)
//   BC4  un canal (el bloque de alfa del BC3)
//   BC5  dos canales (dos bloques BC4)
//   BC7  8 modos con particiones de 1-3 subconjuntos, p-bits y rotacion.

#include "CramionFX/asset/Dds.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace cramion::asset {

namespace {

using Texel = std::array<std::uint8_t, 4>;
using Block = std::array<Texel, 16>;

// --- BC1-BC5 ------------------------------------------------------------------

Texel expand565(unsigned c) {
    const unsigned r = (c >> 11) & 31u;
    const unsigned g = (c >> 5) & 63u;
    const unsigned b = c & 31u;
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)), static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2)), 255};
}

// Bloque de color de 8 bytes. `three_color`: BC1 con c0 <= c1 da 3 colores y
// un texel transparente; en BC2/BC3 el bloque siempre es de 4 colores.
void decodeColor(const std::uint8_t* b, Block& out, bool allow_three_color) {
    const unsigned c0 = b[0] | (b[1] << 8);
    const unsigned c1 = b[2] | (b[3] << 8);
    std::array<Texel, 4> palette{expand565(c0), expand565(c1), Texel{}, Texel{}};
    if (c0 > c1 || !allow_three_color) {
        for (int ch = 0; ch < 3; ++ch) {
            palette[2][ch] = static_cast<std::uint8_t>((2 * palette[0][ch] + palette[1][ch] + 1) / 3);
            palette[3][ch] = static_cast<std::uint8_t>((palette[0][ch] + 2 * palette[1][ch] + 1) / 3);
        }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (int ch = 0; ch < 3; ++ch) {
            palette[2][ch] = static_cast<std::uint8_t>((palette[0][ch] + palette[1][ch] + 1) / 2);
        }
        palette[2][3] = 255;
        palette[3] = Texel{0, 0, 0, 0};
    }
    const std::uint32_t indices = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<std::uint32_t>(b[7]) << 24);
    for (unsigned i = 0; i < 16; ++i) {
        const Texel& c = palette[(indices >> (2 * i)) & 3u];
        out[i][0] = c[0];
        out[i][1] = c[1];
        out[i][2] = c[2];
        out[i][3] = c[3];
    }
}

// Bloque de un canal de 8 bytes (alfa del BC3, BC4, cada canal del BC5).
void decodeChannel(const std::uint8_t* b, Block& out, int channel) {
    const unsigned a0 = b[0];
    const unsigned a1 = b[1];
    std::array<unsigned, 8> palette{a0, a1, 0, 0, 0, 0, 0, 0};
    if (a0 > a1) {
        for (unsigned k = 1; k <= 6; ++k) palette[k + 1] = ((7 - k) * a0 + k * a1 + 3) / 7;
    } else {
        for (unsigned k = 1; k <= 4; ++k) palette[k + 1] = ((5 - k) * a0 + k * a1 + 2) / 5;
        palette[6] = 0;
        palette[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int j = 0; j < 6; ++j) bits |= static_cast<std::uint64_t>(b[2 + j]) << (8 * j);
    for (unsigned i = 0; i < 16; ++i) {
        out[i][channel] = static_cast<std::uint8_t>(palette[static_cast<std::size_t>((bits >> (3 * i)) & 7u)]);
    }
}

// --- BC7 ------------------------------------------------------------------------

struct Bc7Mode {
    int subsets;
    int partition_bits;
    int rotation_bits;
    int selection_bits;
    int color_bits;
    int alpha_bits;
    int endpoint_pbits;  // un p-bit por extremo
    int shared_pbits;    // un p-bit por subconjunto
    int index_bits;
    int index2_bits;     // segundo juego de indices (modos 4 y 5)
};

constexpr std::array<Bc7Mode, 8> kModes = {{
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
    {2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
    {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
    {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
    {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
}};

// Particiones de 2 subconjuntos: bit i = subconjunto del texel i.
constexpr std::array<std::uint16_t, 64> kPartition2 = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
    0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110,
    0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696,
    0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720,
    0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};

// Particiones de 3 subconjuntos: el subconjunto de cada texel.
constexpr std::uint8_t kPartition3[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2},
    {0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2}, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2},
    {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2}, {0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2},
    {0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2}, {0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0},
    {0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2}, {0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0},
    {0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0}, {0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2},
    {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0}, {0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1},
    {0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2}, {0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2},
    {0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1}, {0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2}, {0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0}, {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0},
    {0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0}, {0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1}, {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1}, {0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1}, {0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1}, {0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2}, {0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1},
    {0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2}, {0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2}, {0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2},
    {0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2},
    {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1}, {0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0},
};

// Texel "ancla" de cada subconjunto (su indice lleva un bit menos). El del
// subconjunto 0 es siempre el texel 0.
constexpr std::array<std::uint8_t, 64> kAnchor2 = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8,  2, 2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15,
};
constexpr std::array<std::uint8_t, 64> kAnchor3a = {
    3,  3, 15, 15, 8,  3,  15, 15, 8, 8,  6,  6,  6,  5,  3,  3, 3,  3,  8,  15, 3,  3,
    6,  10, 5, 8,  8,  6,  8,  5,  15, 15, 8, 15, 3,  5,  6,  10, 8, 15, 15, 3,  15, 5,
    15, 15, 15, 15, 3, 15, 5, 5,  5,  8,  5, 10, 5, 10, 8,  13, 15, 12, 3,  3,
};
constexpr std::array<std::uint8_t, 64> kAnchor3b = {
    15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
    15, 8,  3,  15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
    3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};

constexpr std::array<std::uint8_t, 4> kWeights2 = {0, 21, 43, 64};
constexpr std::array<std::uint8_t, 8> kWeights3 = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::array<std::uint8_t, 16> kWeights4 = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

unsigned weight(int bits, unsigned index) {
    if (bits == 2) return kWeights2[index & 3u];
    if (bits == 3) return kWeights3[index & 7u];
    return kWeights4[index & 15u];
}

std::uint8_t interpolate(unsigned a, unsigned b, unsigned w) {
    return static_cast<std::uint8_t>(((64 - w) * a + w * b + 32) >> 6);
}

// Bits del bloque de menor a mayor (el bit 0 es el bit bajo del byte 0).
struct BitReader {
    const std::uint8_t* data;
    unsigned position;
    unsigned read(int count) {
        unsigned value = 0;
        for (int i = 0; i < count; ++i) {
            const unsigned bit = position + static_cast<unsigned>(i);
            value |= ((data[bit >> 3] >> (bit & 7u)) & 1u) << i;
        }
        position += static_cast<unsigned>(count);
        return value;
    }
};

// De `precision` bits a 8, repitiendo los altos en los bajos.
unsigned expandBits(unsigned value, int precision) {
    value <<= (8 - precision);
    return (value | (value >> precision)) & 255u;
}

void decodeBc7(const std::uint8_t* block, Block& out) {
    int mode = 0;
    while (mode < 8 && (block[0] & (1u << mode)) == 0) ++mode;
    if (mode == 8) {  // modo reservado: negro transparente (como la GPU)
        for (Texel& t : out) t = Texel{0, 0, 0, 0};
        return;
    }
    const Bc7Mode& m = kModes[static_cast<std::size_t>(mode)];
    BitReader bits{block, static_cast<unsigned>(mode + 1)};
    const unsigned partition = bits.read(m.partition_bits);
    const unsigned rotation = bits.read(m.rotation_bits);
    const unsigned selection = bits.read(m.selection_bits);

    // Extremos: por canal (R, G, B, A), los de cada subconjunto seguidos.
    const int endpoints = m.subsets * 2;
    unsigned ep[6][4] = {};
    for (int c = 0; c < 3; ++c) {
        for (int e = 0; e < endpoints; ++e) ep[e][c] = bits.read(m.color_bits);
    }
    if (m.alpha_bits > 0) {
        for (int e = 0; e < endpoints; ++e) ep[e][3] = bits.read(m.alpha_bits);
    }
    int color_precision = m.color_bits;
    int alpha_precision = m.alpha_bits;
    if (m.endpoint_pbits != 0 || m.shared_pbits != 0) {
        unsigned pbits[6] = {};
        if (m.endpoint_pbits != 0) {
            for (int e = 0; e < endpoints; ++e) pbits[e] = bits.read(1);
        } else {
            for (int s = 0; s < m.subsets; ++s) pbits[2 * s] = pbits[2 * s + 1] = bits.read(1);
        }
        for (int e = 0; e < endpoints; ++e) {
            for (int c = 0; c < 4; ++c) ep[e][c] = (ep[e][c] << 1) | pbits[e];
        }
        ++color_precision;
        if (alpha_precision > 0) ++alpha_precision;
    }
    for (int e = 0; e < endpoints; ++e) {
        for (int c = 0; c < 3; ++c) ep[e][c] = expandBits(ep[e][c], color_precision);
        ep[e][3] = alpha_precision > 0 ? expandBits(ep[e][3], alpha_precision) : 255u;
    }

    // Subconjunto de cada texel y sus anclas.
    const auto subset_of = [&](unsigned texel) -> unsigned {
        if (m.subsets == 2) return (kPartition2[partition] >> texel) & 1u;
        if (m.subsets == 3) return kPartition3[partition][texel];
        return 0;
    };
    const unsigned anchor1 = m.subsets == 2 ? kAnchor2[partition] : m.subsets == 3 ? kAnchor3a[partition] : 0;
    const unsigned anchor2 = m.subsets == 3 ? kAnchor3b[partition] : 0;
    const auto is_anchor = [&](unsigned texel) {
        return texel == 0 || (m.subsets >= 2 && texel == anchor1) || (m.subsets == 3 && texel == anchor2);
    };

    unsigned index1[16] = {};
    unsigned index2[16] = {};
    for (unsigned i = 0; i < 16; ++i) index1[i] = bits.read(is_anchor(i) ? m.index_bits - 1 : m.index_bits);
    if (m.index2_bits > 0) {
        for (unsigned i = 0; i < 16; ++i) index2[i] = bits.read(i == 0 ? m.index2_bits - 1 : m.index2_bits);
    }

    for (unsigned i = 0; i < 16; ++i) {
        const unsigned s = subset_of(i);
        const unsigned* e0 = ep[2 * s];
        const unsigned* e1 = ep[2 * s + 1];
        unsigned color_index = index1[i];
        unsigned alpha_index = index1[i];
        int color_bits = m.index_bits;
        int alpha_bits = m.index_bits;
        if (m.index2_bits > 0) {
            // Modos 4 y 5: el color con un juego de indices y el alfa con el
            // otro (en el 4, el bit de seleccion los intercambia).
            alpha_index = index2[i];
            alpha_bits = m.index2_bits;
            if (selection != 0) {
                std::swap(color_index, alpha_index);
                std::swap(color_bits, alpha_bits);
            }
        }
        const unsigned wc = weight(color_bits, color_index);
        const unsigned wa = weight(alpha_bits, alpha_index);
        Texel t{interpolate(e0[0], e1[0], wc), interpolate(e0[1], e1[1], wc), interpolate(e0[2], e1[2], wc),
                interpolate(e0[3], e1[3], wa)};
        // Rotacion (modos 4 y 5): el alfa se guardo en otro canal.
        if (rotation == 1) std::swap(t[0], t[3]);
        else if (rotation == 2) std::swap(t[1], t[3]);
        else if (rotation == 3) std::swap(t[2], t[3]);
        out[i] = t;
    }
}

void decodeBlock(TextureFormat format, const std::uint8_t* block, Block& out) {
    switch (format) {
        case TextureFormat::Bc1: decodeColor(block, out, true); break;
        case TextureFormat::Bc2:
            decodeColor(block + 8, out, false);
            for (unsigned i = 0; i < 16; ++i) {
                const unsigned nibble = (block[i / 2] >> ((i & 1u) * 4)) & 15u;
                out[i][3] = static_cast<std::uint8_t>(nibble * 17);
            }
            break;
        case TextureFormat::Bc3:
            decodeColor(block + 8, out, false);
            decodeChannel(block, out, 3);
            break;
        case TextureFormat::Bc4:
            // Un canal: la GPU lo lee como (r, 0, 0, 1).
            for (Texel& t : out) t = Texel{0, 0, 0, 255};
            decodeChannel(block, out, 0);
            break;
        case TextureFormat::Bc5:
            for (Texel& t : out) t = Texel{0, 0, 0, 255};
            decodeChannel(block, out, 0);
            decodeChannel(block + 8, out, 1);
            break;
        case TextureFormat::Bc7: decodeBc7(block, out); break;
        case TextureFormat::Rgba8: break;
    }
}

}  // namespace

std::vector<std::uint8_t> decodeBlockCompressed(const TextureData& texture) {
    const std::uint32_t block_bytes = blockBytes(texture.format);
    if (block_bytes == 0 || texture.width == 0 || texture.height == 0) return {};
    const std::uint32_t blocks_x = std::max(1u, (texture.width + 3) / 4);
    const std::uint32_t blocks_y = std::max(1u, (texture.height + 3) / 4);
    if (texture.pixels.size() < static_cast<std::size_t>(blocks_x) * blocks_y * block_bytes) return {};

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(texture.width) * texture.height * 4);
    Block decoded{};
    for (std::uint32_t by = 0; by < blocks_y; ++by) {
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            const std::uint8_t* block =
                texture.pixels.data() + (static_cast<std::size_t>(by) * blocks_x + bx) * block_bytes;
            decodeBlock(texture.format, block, decoded);
            // Los bloques del borde pueden salirse de la imagen (lados que no
            // son multiplo de 4).
            for (std::uint32_t y = 0; y < 4; ++y) {
                const std::uint32_t py = by * 4 + y;
                if (py >= texture.height) break;
                for (std::uint32_t x = 0; x < 4; ++x) {
                    const std::uint32_t px = bx * 4 + x;
                    if (px >= texture.width) break;
                    std::memcpy(&rgba[(static_cast<std::size_t>(py) * texture.width + px) * 4], decoded[y * 4 + x].data(), 4);
                }
            }
        }
    }
    return rgba;
}

}  // namespace cramion::asset
