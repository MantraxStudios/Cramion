// Texturas de movil: ASTC 4x4 (astc-encoder de Arm, solo en el editor) y ETC2
// RGBA8 (codificador propio, abajo), el archivo .crtx y la cache del juego.
// Ver MobileTextures.h.
//
// ETC2 RGBA8 = 8 bytes de alfa (EAC) + 8 bytes de color (ETC2 RGB). El
// codificador solo usa los modos de ETC1 del color (individual y
// diferencial, con su volteo): son validos en ETC2 y bastan para el respaldo
// (la calidad buena es la de ASTC). El decodificador entiende todos (T, H y
// plano tambien), como la GPU.

#include "CramionFX/asset/MobileTextures.h"

#include "CramionFX/asset/Dds.h"
#include "CramionFX/asset/TextureCompression.h"

#if defined(CRAMION_HAS_ASTC_ENCODER)
#include <astcenc.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

namespace cramion::asset {

namespace {

// Sube si cambia lo que sale de la conversion: lo convertido antes deja de
// valer en la cache del editor.
constexpr std::uint32_t kMobileVersion = 1;

std::uint64_t mixKey(std::uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

std::uint8_t clamp255(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }

unsigned workerCount(unsigned threads, std::size_t rows) {
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    return static_cast<unsigned>(std::clamp<std::size_t>(std::min<std::size_t>(threads, rows), 1, 32));
}

// Reparte las filas de bloques entre hilos.
template <typename Fn>
void forRows(std::uint32_t rows, unsigned threads, Fn&& fn) {
    const unsigned count = workerCount(threads, rows);
    if (count <= 1) {
        for (std::uint32_t r = 0; r < rows; ++r) fn(r);
        return;
    }
    std::atomic<std::uint32_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(count);
    for (unsigned t = 0; t < count; ++t) {
        pool.emplace_back([&] {
            for (std::uint32_t r = next++; r < rows; r = next++) fn(r);
        });
    }
    for (std::thread& t : pool) t.join();
}

// Un bloque de 4x4 de la imagen (los del borde repiten el ultimo texel).
void gatherBlock(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height, std::uint32_t bx,
                 std::uint32_t by, std::uint8_t out[64]) {
    for (std::uint32_t y = 0; y < 4; ++y) {
        const std::uint32_t py = std::min(by * 4 + y, height - 1);
        for (std::uint32_t x = 0; x < 4; ++x) {
            const std::uint32_t px = std::min(bx * 4 + x, width - 1);
            std::memcpy(out + (y * 4 + x) * 4, rgba + (static_cast<std::size_t>(py) * width + px) * 4, 4);
        }
    }
}

void scatterBlock(const std::uint8_t block[64], std::uint32_t width, std::uint32_t height, std::uint32_t bx,
                  std::uint32_t by, std::uint8_t* rgba) {
    for (std::uint32_t y = 0; y < 4; ++y) {
        const std::uint32_t py = by * 4 + y;
        if (py >= height) break;
        for (std::uint32_t x = 0; x < 4; ++x) {
            const std::uint32_t px = bx * 4 + x;
            if (px >= width) break;
            std::memcpy(rgba + (static_cast<std::size_t>(py) * width + px) * 4, block + (y * 4 + x) * 4, 4);
        }
    }
}

// --- ETC1 / ETC2 (color) --------------------------------------------------------

constexpr int kEtcModifiers[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
constexpr int kEtcDistances[8] = {3, 6, 11, 16, 23, 32, 41, 64};

// El desplazamiento de cada indice de texel (msb, lsb): 0 +a, 1 +b, 2 -a, 3 -b.
int etcOffset(int table, int index) {
    const int v = kEtcModifiers[table][index & 1];
    return (index & 2) != 0 ? -v : v;
}

int extend4(int c) { return (c << 4) | c; }
int extend5(int c) { return (c << 3) | (c >> 2); }
int extend6(int c) { return (c << 2) | (c >> 4); }
int extend7(int c) { return (c << 1) | (c >> 6); }
int signed3(int v) { return (v & 4) != 0 ? v - 8 : v; }

std::uint32_t readBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
void writeBe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

// El indice de 2 bits del texel (x, y): los texeles van por columnas (x * 4 + y).
int etcIndex(std::uint32_t lo, int x, int y) {
    const int i = x * 4 + y;
    return static_cast<int>(((lo >> (i + 16)) & 1u) << 1 | ((lo >> i) & 1u));
}

void decodeEtc2Color(const std::uint8_t* block, std::uint8_t rgba[64]) {
    const std::uint32_t hi = readBe32(block);
    const std::uint32_t lo = readBe32(block + 4);
    const bool diff = (hi >> 1) & 1u;
    const bool flip = hi & 1u;
    const auto put = [&](int x, int y, int r, int g, int b) {
        std::uint8_t* p = rgba + (y * 4 + x) * 4;
        p[0] = clamp255(r);
        p[1] = clamp255(g);
        p[2] = clamp255(b);
    };
    int base[2][3];
    if (!diff) {
        base[0][0] = extend4((hi >> 28) & 15);
        base[1][0] = extend4((hi >> 24) & 15);
        base[0][1] = extend4((hi >> 20) & 15);
        base[1][1] = extend4((hi >> 16) & 15);
        base[0][2] = extend4((hi >> 12) & 15);
        base[1][2] = extend4((hi >> 8) & 15);
    } else {
        const int r = static_cast<int>((hi >> 27) & 31), dr = signed3((hi >> 24) & 7);
        const int g = static_cast<int>((hi >> 19) & 31), dg = signed3((hi >> 16) & 7);
        const int b = static_cast<int>((hi >> 11) & 31), db = signed3((hi >> 8) & 7);
        if (r + dr < 0 || r + dr > 31) {
            // Modo T: dos colores de 4 bits; el segundo con +-distancia.
            const int r1 = static_cast<int>(((hi >> 27) & 3) << 2 | ((hi >> 24) & 3));
            const int c1[3] = {extend4(r1), extend4((hi >> 20) & 15), extend4((hi >> 16) & 15)};
            const int c2[3] = {extend4((hi >> 12) & 15), extend4((hi >> 8) & 15), extend4((hi >> 4) & 15)};
            const int d = kEtcDistances[((hi >> 2) & 3) << 1 | (hi & 1)];
            for (int x = 0; x < 4; ++x) {
                for (int y = 0; y < 4; ++y) {
                    switch (etcIndex(lo, x, y)) {
                        case 0: put(x, y, c1[0], c1[1], c1[2]); break;
                        case 1: put(x, y, c2[0] + d, c2[1] + d, c2[2] + d); break;
                        case 2: put(x, y, c2[0], c2[1], c2[2]); break;
                        default: put(x, y, c2[0] - d, c2[1] - d, c2[2] - d); break;
                    }
                }
            }
            return;
        }
        if (g + dg < 0 || g + dg > 31) {
            // Modo H: dos colores de 4 bits, cada uno +-distancia.
            const int r1 = static_cast<int>((hi >> 27) & 15);
            const int g1 = static_cast<int>(((hi >> 24) & 7) << 1 | ((hi >> 20) & 1));
            const int b1 = static_cast<int>(((hi >> 19) & 1) << 3 | ((hi >> 15) & 7));
            const int r2 = static_cast<int>((hi >> 11) & 15);
            const int g2 = static_cast<int>((hi >> 7) & 15);
            const int b2 = static_cast<int>((hi >> 3) & 15);
            const int c1[3] = {extend4(r1), extend4(g1), extend4(b1)};
            const int c2[3] = {extend4(r2), extend4(g2), extend4(b2)};
            const int v1 = (r1 << 8) | (g1 << 4) | b1;
            const int v2 = (r2 << 8) | (g2 << 4) | b2;
            const int d = kEtcDistances[(((hi >> 2) & 1) << 2) | ((hi & 1) << 1) | (v1 >= v2 ? 1 : 0)];
            for (int x = 0; x < 4; ++x) {
                for (int y = 0; y < 4; ++y) {
                    switch (etcIndex(lo, x, y)) {
                        case 0: put(x, y, c1[0] + d, c1[1] + d, c1[2] + d); break;
                        case 1: put(x, y, c1[0] - d, c1[1] - d, c1[2] - d); break;
                        case 2: put(x, y, c2[0] + d, c2[1] + d, c2[2] + d); break;
                        default: put(x, y, c2[0] - d, c2[1] - d, c2[2] - d); break;
                    }
                }
            }
            return;
        }
        if (b + db < 0 || b + db > 31) {
            // Modo plano: origen, horizontal y vertical (interpolacion).
            const std::uint64_t bits = (static_cast<std::uint64_t>(hi) << 32) | lo;
            const auto field = [&](int high, int count) { return static_cast<int>((bits >> (high - count + 1)) & ((1u << count) - 1)); };
            const int ro = extend6(field(62, 6));
            const int go = extend7((field(56, 1) << 6) | field(54, 6));
            const int bo = extend6((field(48, 1) << 5) | (field(44, 2) << 3) | field(41, 3));
            const int rh = extend6((field(38, 5) << 1) | field(32, 1));
            const int gh = extend7(field(31, 7));
            const int bh = extend6(field(24, 6));
            const int rv = extend6(field(18, 6));
            const int gv = extend7(field(12, 7));
            const int bv = extend6(field(5, 6));
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    put(x, y, (x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2,
                        (x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2,
                        (x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2);
                }
            }
            return;
        }
        base[0][0] = extend5(r);
        base[1][0] = extend5(r + dr);
        base[0][1] = extend5(g);
        base[1][1] = extend5(g + dg);
        base[0][2] = extend5(b);
        base[1][2] = extend5(b + db);
    }
    const int table[2] = {static_cast<int>((hi >> 5) & 7), static_cast<int>((hi >> 2) & 7)};
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            const int sub = flip ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
            const int o = etcOffset(table[sub], etcIndex(lo, x, y));
            put(x, y, base[sub][0] + o, base[sub][1] + o, base[sub][2] + o);
        }
    }
}

// Error de un subbloque con un color base y una tabla; deja los indices.
int subblockError(const std::uint8_t block[64], const int* texels, int count, const int base[3], int table,
                  int* indices) {
    int total = 0;
    for (int k = 0; k < count; ++k) {
        const std::uint8_t* p = block + texels[k] * 4;
        int best = 1 << 30;
        int best_index = 0;
        for (int index = 0; index < 4; ++index) {
            const int o = etcOffset(table, index);
            const int dr = std::clamp(base[0] + o, 0, 255) - p[0];
            const int dg = std::clamp(base[1] + o, 0, 255) - p[1];
            const int db = std::clamp(base[2] + o, 0, 255) - p[2];
            const int e = dr * dr * 3 + dg * dg * 6 + db * db;  // mas peso al verde (la luminancia)
            if (e < best) {
                best = e;
                best_index = index;
            }
        }
        total += best;
        indices[k] = best_index;
    }
    return total;
}

void encodeEtc1Color(const std::uint8_t block[64], std::uint8_t out[8]) {
    int best_error = 1 << 30;
    std::uint32_t best_hi = 0;
    std::uint32_t best_lo = 0;
    for (int flip = 0; flip < 2; ++flip) {
        int texels[2][8];
        int counts[2] = {0, 0};
        float average[2][3] = {};
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const int sub = flip ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
                const int t = y * 4 + x;
                texels[sub][counts[sub]++] = t;
                for (int c = 0; c < 3; ++c) average[sub][c] += block[t * 4 + c];
            }
        }
        for (int s = 0; s < 2; ++s) {
            for (int c = 0; c < 3; ++c) average[s][c] /= 8.0f;
        }
        for (int differential = 0; differential < 2; ++differential) {
            int q[2][3];
            int base[2][3];
            for (int s = 0; s < 2; ++s) {
                for (int c = 0; c < 3; ++c) {
                    q[s][c] = differential ? std::clamp(static_cast<int>(std::lround(average[s][c] * 31.0f / 255.0f)), 0, 31)
                                           : std::clamp(static_cast<int>(std::lround(average[s][c] * 15.0f / 255.0f)), 0, 15);
                }
            }
            if (differential) {
                // La diferencia cabe en 3 bits con signo (-4..3): si no, el
                // segundo color se acerca al primero (sigue siendo valido).
                for (int c = 0; c < 3; ++c) q[1][c] = std::clamp(q[1][c], q[0][c] - 4, q[0][c] + 3);
            }
            for (int s = 0; s < 2; ++s) {
                for (int c = 0; c < 3; ++c) base[s][c] = differential ? extend5(q[s][c]) : extend4(q[s][c]);
            }
            int error = 0;
            int tables[2] = {0, 0};
            int indices[2][8];
            for (int s = 0; s < 2; ++s) {
                int best_sub = 1 << 30;
                for (int table = 0; table < 8; ++table) {
                    int trial[8];
                    const int e = subblockError(block, texels[s], counts[s], base[s], table, trial);
                    if (e < best_sub) {
                        best_sub = e;
                        tables[s] = table;
                        std::memcpy(indices[s], trial, sizeof(trial));
                    }
                }
                error += best_sub;
            }
            if (error >= best_error) continue;
            best_error = error;
            std::uint32_t hi = 0;
            if (differential) {
                hi = static_cast<std::uint32_t>(q[0][0]) << 27 | static_cast<std::uint32_t>((q[1][0] - q[0][0]) & 7) << 24 |
                     static_cast<std::uint32_t>(q[0][1]) << 19 | static_cast<std::uint32_t>((q[1][1] - q[0][1]) & 7) << 16 |
                     static_cast<std::uint32_t>(q[0][2]) << 11 | static_cast<std::uint32_t>((q[1][2] - q[0][2]) & 7) << 8 |
                     2u;
            } else {
                hi = static_cast<std::uint32_t>(q[0][0]) << 28 | static_cast<std::uint32_t>(q[1][0]) << 24 |
                     static_cast<std::uint32_t>(q[0][1]) << 20 | static_cast<std::uint32_t>(q[1][1]) << 16 |
                     static_cast<std::uint32_t>(q[0][2]) << 12 | static_cast<std::uint32_t>(q[1][2]) << 8;
            }
            hi |= static_cast<std::uint32_t>(tables[0]) << 5 | static_cast<std::uint32_t>(tables[1]) << 2 |
                  static_cast<std::uint32_t>(flip);
            std::uint32_t lo = 0;
            for (int s = 0; s < 2; ++s) {
                for (int k = 0; k < counts[s]; ++k) {
                    const int t = texels[s][k];
                    const int i = (t % 4) * 4 + t / 4;  // por columnas
                    lo |= static_cast<std::uint32_t>((indices[s][k] >> 1) & 1) << (i + 16);
                    lo |= static_cast<std::uint32_t>(indices[s][k] & 1) << i;
                }
            }
            best_hi = hi;
            best_lo = lo;
        }
    }
    writeBe32(out, best_hi);
    writeBe32(out + 4, best_lo);
}

// --- EAC (alfa de 8 bits) -------------------------------------------------------

constexpr int kEacTables[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10}, {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},  {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},  {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8}};

void decodeEac(const std::uint8_t* block, std::uint8_t rgba[64]) {
    const int base = block[0];
    const int multiplier = block[1] >> 4;
    const int table = block[1] & 15;
    std::uint64_t bits = 0;
    for (int i = 2; i < 8; ++i) bits = (bits << 8) | block[i];
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            const int i = x * 4 + y;
            const int index = static_cast<int>((bits >> (45 - 3 * i)) & 7);
            rgba[(y * 4 + x) * 4 + 3] = clamp255(base + kEacTables[table][index] * multiplier);
        }
    }
}

void encodeEac(const std::uint8_t block[64], std::uint8_t out[8]) {
    int low = 255;
    int high = 0;
    for (int t = 0; t < 16; ++t) {
        low = std::min<int>(low, block[t * 4 + 3]);
        high = std::max<int>(high, block[t * 4 + 3]);
    }
    int best_base = low;
    int best_multiplier = 1;
    int best_table = 13;  // su indice 4 es 0: un alfa constante sale exacto
    int best_indices[16] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
    if (low != high) {
        int best_error = 1 << 30;
        for (int table = 0; table < 16; ++table) {
            const int* tbl = kEacTables[table];
            const int span = tbl[7] - tbl[3];
            const int m0 = static_cast<int>(std::lround(static_cast<float>(high - low) / static_cast<float>(span)));
            for (int multiplier = std::max(1, m0 - 1); multiplier <= std::min(15, m0 + 1); ++multiplier) {
                const int candidates[2] = {
                    std::clamp(static_cast<int>(std::lround(low - tbl[3] * multiplier)), 0, 255),
                    std::clamp(static_cast<int>(std::lround((low + high) * 0.5f - (tbl[7] + tbl[3]) * multiplier * 0.5f)), 0, 255)};
                for (const int base : candidates) {
                    for (int delta = -1; delta <= 1; ++delta) {
                        const int b = std::clamp(base + delta, 0, 255);
                        int error = 0;
                        int indices[16];
                        for (int t = 0; t < 16 && error < best_error; ++t) {
                            const int a = block[t * 4 + 3];
                            int best = 1 << 30;
                            for (int index = 0; index < 8; ++index) {
                                const int d = std::clamp(b + tbl[index] * multiplier, 0, 255) - a;
                                if (d * d < best) {
                                    best = d * d;
                                    indices[t] = index;
                                }
                            }
                            error += best;
                        }
                        if (error < best_error) {
                            best_error = error;
                            best_base = b;
                            best_multiplier = multiplier;
                            best_table = table;
                            std::memcpy(best_indices, indices, sizeof(indices));
                        }
                    }
                }
            }
        }
    }
    out[0] = static_cast<std::uint8_t>(best_base);
    out[1] = static_cast<std::uint8_t>(best_multiplier << 4 | best_table);
    std::uint64_t bits = 0;
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            const int i = x * 4 + y;
            bits |= static_cast<std::uint64_t>(best_indices[y * 4 + x]) << (45 - 3 * i);
        }
    }
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<std::uint8_t>(bits >> (40 - 8 * i));
}

// --- Cache del juego ------------------------------------------------------------

std::mutex g_cache_mutex;
std::filesystem::path g_cache_folder;
std::filesystem::path g_cache_root;
bool g_cache_astc = false;
bool g_cache_etc2 = false;
std::atomic<bool> g_cache_active{false};
std::atomic<std::uint32_t> g_loaded{0};
std::atomic<std::uint32_t> g_looked_up{0};

std::string keyName(std::uint64_t key, MobileFormat format) {
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx%s", static_cast<unsigned long long>(key), mobileTextureExtension(format));
    return name;
}

bool readWholeFile(const std::filesystem::path& file, std::vector<std::uint8_t>& out) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size <= 0) return false;
    out.resize(static_cast<std::size_t>(size));
    in.seekg(0);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
}

// Los mips de un nivel RGBA8 (media de 2x2), para lo que llega sin ellos.
std::vector<std::vector<std::uint8_t>> rgbaMipChain(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height) {
    std::vector<std::vector<std::uint8_t>> levels;
    levels.emplace_back(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
    std::uint32_t w = width;
    std::uint32_t h = height;
    while (w > 1 || h > 1) {
        const std::uint32_t nw = std::max(w / 2, 1u);
        const std::uint32_t nh = std::max(h / 2, 1u);
        const std::vector<std::uint8_t>& src = levels.back();
        std::vector<std::uint8_t> dst(static_cast<std::size_t>(nw) * nh * 4);
        for (std::uint32_t y = 0; y < nh; ++y) {
            for (std::uint32_t x = 0; x < nw; ++x) {
                for (int c = 0; c < 4; ++c) {
                    unsigned sum = 0;
                    for (std::uint32_t dy = 0; dy < 2; ++dy) {
                        for (std::uint32_t dx = 0; dx < 2; ++dx) {
                            const std::uint32_t sx = std::min(x * 2 + dx, w - 1);
                            const std::uint32_t sy = std::min(y * 2 + dy, h - 1);
                            sum += src[(static_cast<std::size_t>(sy) * w + sx) * 4 + c];
                        }
                    }
                    dst[(static_cast<std::size_t>(y) * nw + x) * 4 + c] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
        }
        levels.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
    return levels;
}

}  // namespace

// -----------------------------------------------------------------------------

TextureFormat mobileTextureFormat(MobileFormat format) {
    return format == MobileFormat::Astc4x4 ? TextureFormat::Astc4x4 : TextureFormat::Etc2Rgba8;
}

const char* mobileTextureExtension(MobileFormat format) { return format == MobileFormat::Astc4x4 ? ".astc" : ".etc2"; }

bool isMobileTextureFormat(TextureFormat format) {
    return format == TextureFormat::Astc4x4 || format == TextureFormat::Etc2Rgba8;
}

void decodeEtc2Rgba8Block(const std::uint8_t* block, std::uint8_t rgba[64]) {
    decodeEtc2Color(block + 8, rgba);
    decodeEac(block, rgba);
}

std::vector<std::uint8_t> encodeEtc2Rgba8Level(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                                               unsigned threads) {
    if (rgba == nullptr || width == 0 || height == 0) return {};
    const std::uint32_t bw = (width + 3) / 4;
    const std::uint32_t bh = (height + 3) / 4;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bw) * bh * 16);
    forRows(bh, threads, [&](std::uint32_t by) {
        std::uint8_t block[64];
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            gatherBlock(rgba, width, height, bx, by, block);
            std::uint8_t* dst = out.data() + (static_cast<std::size_t>(by) * bw + bx) * 16;
            encodeEac(block, dst);
            encodeEtc1Color(block, dst + 8);
        }
    });
    return out;
}

std::vector<std::uint8_t> decodeEtc2Rgba8Level(const std::uint8_t* blocks, std::size_t size, std::uint32_t width,
                                               std::uint32_t height) {
    const std::uint32_t bw = (width + 3) / 4;
    const std::uint32_t bh = (height + 3) / 4;
    if (blocks == nullptr || width == 0 || height == 0 || size < static_cast<std::size_t>(bw) * bh * 16) return {};
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    std::uint8_t block[64];
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            decodeEtc2Rgba8Block(blocks + (static_cast<std::size_t>(by) * bw + bx) * 16, block);
            scatterBlock(block, width, height, bx, by, rgba.data());
        }
    }
    return rgba;
}

bool astcEncoderAvailable() {
#if defined(CRAMION_HAS_ASTC_ENCODER)
    return true;
#else
    return false;
#endif
}

std::vector<std::uint8_t> encodeAstc4x4Level(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                                             bool normal_map, unsigned threads) {
#if defined(CRAMION_HAS_ASTC_ENCODER)
    if (rgba == nullptr || width == 0 || height == 0) return {};
    astcenc_config config{};
    if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_MEDIUM, 0, &config) != ASTCENC_SUCCESS) return {};
    if (normal_map) {
        // La normal va en R y G (el shader rehace Z): B y A pesan poco.
        config.cw_b_weight = 0.1f;
        config.cw_a_weight = 0.1f;
    }
    const std::uint32_t bw = (width + 3) / 4;
    const std::uint32_t bh = (height + 3) / 4;
    const unsigned count = workerCount(threads, bh);
    astcenc_context* context = nullptr;
    if (astcenc_context_alloc(&config, count, &context) != ASTCENC_SUCCESS) return {};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bw) * bh * 16);
    void* slices[1] = {const_cast<std::uint8_t*>(rgba)};
    astcenc_image image{};
    image.dim_x = width;
    image.dim_y = height;
    image.dim_z = 1;
    image.data_type = ASTCENC_TYPE_U8;
    image.data = slices;
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    std::atomic<bool> failed{false};
    {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < count; ++t) {
            pool.emplace_back([&, t] {
                if (astcenc_compress_image(context, &image, &swizzle, out.data(), out.size(), t) != ASTCENC_SUCCESS) {
                    failed = true;
                }
            });
        }
        for (std::thread& t : pool) t.join();
    }
    astcenc_context_free(context);
    if (failed) return {};
    return out;
#else
    (void)rgba;
    (void)width;
    (void)height;
    (void)normal_map;
    (void)threads;
    return {};
#endif
}

std::vector<std::uint8_t> decodeAstc4x4Level(const std::uint8_t* blocks, std::size_t size, std::uint32_t width,
                                             std::uint32_t height) {
#if defined(CRAMION_HAS_ASTC_ENCODER)
    const std::size_t needed = static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) * 16;
    if (blocks == nullptr || width == 0 || height == 0 || size < needed) return {};
    astcenc_config config{};
    if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, &config) !=
        ASTCENC_SUCCESS) {
        return {};
    }
    astcenc_context* context = nullptr;
    if (astcenc_context_alloc(&config, 1, &context) != ASTCENC_SUCCESS) return {};
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    void* slices[1] = {rgba.data()};
    astcenc_image image{};
    image.dim_x = width;
    image.dim_y = height;
    image.dim_z = 1;
    image.data_type = ASTCENC_TYPE_U8;
    image.data = slices;
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const bool ok = astcenc_decompress_image(context, blocks, needed, &image, &swizzle, 0) == ASTCENC_SUCCESS;
    astcenc_context_free(context);
    if (!ok) return {};
    return rgba;
#else
    (void)blocks;
    (void)size;
    (void)width;
    (void)height;
    return {};
#endif
}

bool convertToMobileTexture(const TextureData& source, MobileFormat format, TextureData& out, unsigned threads) {
    if (source.width == 0 || source.height == 0 || source.pixels.empty()) return false;
    if (format == MobileFormat::Astc4x4 && !astcEncoderAvailable()) return false;
    // Los niveles en RGBA8: los del BC uno a uno (los mips del editor ya se
    // hicieron en espacio lineal y renormalizados); RGBA8, su cadena.
    std::vector<std::vector<std::uint8_t>> levels;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
    if (source.format == TextureFormat::Rgba8) {
        if (source.pixels.size() < static_cast<std::size_t>(source.width) * source.height * 4) return false;
        levels = rgbaMipChain(source.pixels.data(), source.width, source.height);
        std::uint32_t w = source.width;
        std::uint32_t h = source.height;
        for (std::size_t i = 0; i < levels.size(); ++i) {
            sizes.emplace_back(w, h);
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
        }
    } else if (isMobileTextureFormat(source.format)) {
        return false;  // ya lo es
    } else {
        std::uint32_t w = source.width;
        std::uint32_t h = source.height;
        std::size_t offset = 0;
        for (std::uint32_t m = 0; m < std::max(source.mip_levels, 1u); ++m) {
            const std::size_t bytes = mipByteSize(source.format, w, h);
            if (offset + bytes > source.pixels.size()) break;
            TextureData level;
            level.width = w;
            level.height = h;
            level.format = source.format;
            level.pixels.assign(source.pixels.begin() + static_cast<std::ptrdiff_t>(offset),
                                source.pixels.begin() + static_cast<std::ptrdiff_t>(offset + bytes));
            std::vector<std::uint8_t> rgba = decodeBlockCompressed(level);
            if (rgba.empty()) break;
            levels.push_back(std::move(rgba));
            sizes.emplace_back(w, h);
            offset += bytes;
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
        }
        if (levels.empty()) return false;
    }
    TextureData result;
    result.name = source.name;
    result.width = source.width;
    result.height = source.height;
    result.format = mobileTextureFormat(format);
    result.mip_levels = static_cast<std::uint32_t>(levels.size());
    result.usage = source.usage;
    result.height_map = false;  // (ya es un normal map)
    bool cutout = false;
    for (std::size_t i = 3; i < levels[0].size(); i += 4) {
        if (levels[0][i] < 128) {
            cutout = true;
            break;
        }
    }
    result.alpha = static_cast<std::int8_t>(cutout ? 1 : 0);
    const bool normal = source.usage == TextureUsage::Normal;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const auto [w, h] = sizes[i];
        std::vector<std::uint8_t> blocks = format == MobileFormat::Astc4x4
                                               ? encodeAstc4x4Level(levels[i].data(), w, h, normal, threads)
                                               : encodeEtc2Rgba8Level(levels[i].data(), w, h, threads);
        if (blocks.empty()) return false;
        result.pixels.insert(result.pixels.end(), blocks.begin(), blocks.end());
    }
    out = std::move(result);
    return true;
}

// --- .crtx ----------------------------------------------------------------------

std::vector<std::uint8_t> writeMobileTextureFile(const TextureData& texture) {
    std::vector<std::uint8_t> out(32);
    std::memcpy(out.data(), "CRTX", 4);
    const std::uint32_t version = kMobileVersion;
    std::memcpy(out.data() + 4, &version, 4);
    out[8] = static_cast<std::uint8_t>(texture.format == TextureFormat::Astc4x4 ? MobileFormat::Astc4x4
                                                                                : MobileFormat::Etc2Rgba8);
    out[9] = static_cast<std::uint8_t>(texture.alpha);
    out[10] = static_cast<std::uint8_t>(texture.usage);
    std::memcpy(out.data() + 12, &texture.width, 4);
    std::memcpy(out.data() + 16, &texture.height, 4);
    std::memcpy(out.data() + 20, &texture.mip_levels, 4);
    const std::uint64_t bytes = texture.pixels.size();
    std::memcpy(out.data() + 24, &bytes, 8);
    out.insert(out.end(), texture.pixels.begin(), texture.pixels.end());
    return out;
}

bool readMobileTextureFile(const std::uint8_t* data, std::size_t size, TextureData& out) {
    if (data == nullptr || size < 32 || std::memcmp(data, "CRTX", 4) != 0) return false;
    std::uint32_t version = 0;
    std::memcpy(&version, data + 4, 4);
    if (version != kMobileVersion) return false;
    const std::uint8_t format = data[8];
    if (format != static_cast<std::uint8_t>(MobileFormat::Astc4x4) &&
        format != static_cast<std::uint8_t>(MobileFormat::Etc2Rgba8)) {
        return false;
    }
    TextureData texture;
    texture.format = mobileTextureFormat(static_cast<MobileFormat>(format));
    texture.alpha = static_cast<std::int8_t>(data[9]);
    texture.usage = static_cast<TextureUsage>(data[10]);
    std::memcpy(&texture.width, data + 12, 4);
    std::memcpy(&texture.height, data + 16, 4);
    std::memcpy(&texture.mip_levels, data + 20, 4);
    std::uint64_t bytes = 0;
    std::memcpy(&bytes, data + 24, 8);
    if (texture.width == 0 || texture.height == 0 || texture.width > 16384 || texture.height > 16384 ||
        texture.mip_levels == 0 || texture.mip_levels > 16 || bytes != size - 32) {
        return false;
    }
    // Los mips que dice tiene que caber justo.
    std::size_t expected = 0;
    std::uint32_t w = texture.width;
    std::uint32_t h = texture.height;
    for (std::uint32_t m = 0; m < texture.mip_levels; ++m) {
        expected += mipByteSize(texture.format, w, h);
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    if (expected != bytes) return false;
    texture.pixels.assign(data + 32, data + size);
    texture.name = out.name;
    out = std::move(texture);
    return true;
}

bool readMobileTextureFile(const std::filesystem::path& file, TextureData& out) {
    std::vector<std::uint8_t> bytes;
    return readWholeFile(file, bytes) && readMobileTextureFile(bytes.data(), bytes.size(), out);
}

// --- Claves y cache ---------------------------------------------------------------

std::uint64_t mobileTextureKey(const TextureData& stored, const std::filesystem::path& assets_root) {
    if (isLazyTexture(stored)) return lazyTextureKey(stored, assets_root);
    if (!stored.encoded.empty()) {
        if (isDds(stored.encoded.data(), stored.encoded.size())) {
            // Un DDS incrustado se queda en BC en el .crdata: por sus bytes.
            return mixKey(textureCacheKey(stored.encoded.data(), stored.encoded.size(), false, stored.usage) ^ 0xdd5ULL) | 1u;
        }
        return textureCacheKey(stored.encoded.data(), stored.encoded.size(), stored.height_map, stored.usage);
    }
    if (!stored.pixels.empty() && !isMobileTextureFormat(stored.format)) {
        std::uint64_t h = textureCacheKey(stored.pixels.data(), stored.pixels.size(), stored.height_map, stored.usage);
        h = mixKey(h ^ (static_cast<std::uint64_t>(stored.width) << 32 | stored.height));
        h = mixKey(h ^ (0x0b10c0000ULL + static_cast<std::uint64_t>(stored.format)) ^
                   (static_cast<std::uint64_t>(stored.mip_levels) << 40));
        return h | 1u;
    }
    return 0;
}

void setMobileTextureCache(const std::filesystem::path& folder, const std::filesystem::path& assets_root, bool astc,
                           bool etc2) {
    const std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cache_folder = folder;
    g_cache_root = assets_root;
    g_cache_astc = astc;
    g_cache_etc2 = etc2;
    g_cache_active = !folder.empty() && (astc || etc2);
}

bool mobileTextureCacheActive() { return g_cache_active.load(); }

bool loadMobileTexture(const TextureData& stored, TextureData& out) {
    if (!g_cache_active.load()) return false;
    std::filesystem::path folder;
    std::filesystem::path root;
    bool astc = false;
    bool etc2 = false;
    {
        const std::lock_guard<std::mutex> lock(g_cache_mutex);
        folder = g_cache_folder;
        root = g_cache_root;
        astc = g_cache_astc;
        etc2 = g_cache_etc2;
    }
    const std::uint64_t key = mobileTextureKey(stored, root);
    if (key == 0) return false;
    ++g_looked_up;
    for (const MobileFormat format : {MobileFormat::Astc4x4, MobileFormat::Etc2Rgba8}) {
        if ((format == MobileFormat::Astc4x4 && !astc) || (format == MobileFormat::Etc2Rgba8 && !etc2)) continue;
        const std::filesystem::path file = folder / keyName(key, format);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) continue;
        TextureData texture;
        texture.name = stored.name;
        if (!readMobileTextureFile(file, texture)) {
            std::cerr << "[Texturas] " << file.filename().string() << " no es valida (se usa la de siempre)\n";
            continue;
        }
        texture.usage = stored.usage;
        out = std::move(texture);
        ++g_loaded;
        return true;
    }
    return false;
}

void mobileTextureCounts(std::uint32_t& loaded, std::uint32_t& looked_up) {
    loaded = g_loaded.load();
    looked_up = g_looked_up.load();
}

std::uint64_t prepareMobileExportTexture(const TextureData& texture, const std::filesystem::path& assets_root,
                                         MobileFormat format, const std::filesystem::path& cache_folder,
                                         std::filesystem::path& file) {
    file.clear();
    if (cache_folder.empty() || (format == MobileFormat::Astc4x4 && !astcEncoderAvailable())) return 0;
    const std::uint64_t key = mobileTextureKey(texture, assets_root);
    if (key == 0) return 0;
    // El origen: los pixeles que trae el .crdata (BC o RGBA8) o el .dds de la
    // cache del editor (prepareExportTexture lo comprime si falta).
    TextureData source;
    std::uint64_t cache_id = 0;
    if (!texture.pixels.empty() && !isMobileTextureFormat(texture.format)) {
        if (std::max(texture.width, texture.height) < 16) return 0;
        source = texture;
        cache_id = key;
    } else if (!texture.encoded.empty() && isDds(texture.encoded.data(), texture.encoded.size())) {
        if (!parseDds(texture.encoded.data(), texture.encoded.size(), source)) return 0;
        cache_id = key;
    } else {
        std::filesystem::path dds;
        const std::uint64_t portable = prepareExportTexture(texture, assets_root, dds);
        if (portable == 0 || portable != key || dds.empty()) return 0;
        // La cache del editor cambia de nombre si cambia la imagen (su clave
        // lleva la fecha): la de movil va por ese nombre, su tamano y su fecha.
        std::error_code ec;
        const std::u8string name = dds.filename().u8string();
        cache_id = textureCacheKey(reinterpret_cast<const std::uint8_t*>(name.data()), name.size(), false);
        cache_id = mixKey(cache_id ^ static_cast<std::uint64_t>(std::filesystem::file_size(dds, ec)));
        cache_id = mixKey(cache_id ^ static_cast<std::uint64_t>(std::filesystem::last_write_time(dds, ec).time_since_epoch().count()));
        std::vector<std::uint8_t> bytes;
        if (!readWholeFile(dds, bytes) || !parseDds(bytes.data(), bytes.size(), source)) return 0;
        if (bytes.size() > 4 + 124 + 20) {
            std::uint32_t alpha_mode = 0;
            std::memcpy(&alpha_mode, bytes.data() + 4 + 124 + 16, 4);
            source.alpha = alpha_mode == 3 ? 0 : 1;
        }
    }
    source.usage = texture.usage;
    source.name = texture.name;
    cache_id = mixKey(cache_id ^ (static_cast<std::uint64_t>(kMobileVersion) << 48) ^
                      static_cast<std::uint64_t>(format)) | 1u;
    std::error_code ec;
    std::filesystem::create_directories(cache_folder, ec);
    const std::filesystem::path cached = cache_folder / keyName(cache_id, format);
    if (std::filesystem::is_regular_file(cached, ec)) {
        file = cached;
        return key;
    }
    TextureData converted;
    if (!convertToMobileTexture(source, format, converted)) return 0;
    const std::vector<std::uint8_t> bytes = writeMobileTextureFile(converted);
    std::filesystem::path temporary = cached;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return 0;
    }
    std::filesystem::rename(temporary, cached, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return 0;
    }
    file = cached;
    return key;
}

}  // namespace cramion::asset
