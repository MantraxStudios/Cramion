// cramion_dimension_preview: genera una zona de una dimension del mundo de
// bloques y la escribe como OBJ con color por vertice (el estilo de color
// liso del juego), para mirarla en Blender sin abrir el motor.
//
//   cramion_dimension_preview <dimension> <salida.obj> [cx cz chunks y_corte semilla]
//
//   dimension  superficie | abismo | cielo | cristal
//   cx cz      chunk de la esquina (por defecto -4 -4)
//   chunks     lado de la zona en chunks (por defecto 8)
//   y_corte    no escribe nada por encima (el techo del abismo; 256 = nada)
//
// Dos objetos: "solid" (bloques normales) y "glow" (los que dan luz: lava,
// cristales, hongos...), para darles un material que emite.

#include "CramionCore/voxel/Voxel.h"

#include "../src/voxel/VoxelInternal.h"

#include <CramionFX/vk/VoxelPass.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace cramion;
using namespace cramion::voxel;

namespace {

struct Rgb {
    float r, g, b;
};

Rgb flatColor(BlockId id, int face) {
    static std::unordered_map<std::string, Rgb> cache;
    const BlockDef& def = blockDef(id);
    const std::string& name = face == 2 ? def.top : (face == 3 ? def.bottom : def.side);
    if (const auto it = cache.find(name); it != cache.end()) return it->second;
    gfx::VoxelTextureLayer layer;
    buildTexture(name, 16, {}, layer, false);
    double r = 0, g = 0, b = 0, n = 0;
    for (std::size_t i = 0; i + 3 < layer.albedo.size(); i += 4) {
        if (layer.albedo[i + 3] < 128) continue;
        r += layer.albedo[i];
        g += layer.albedo[i + 1];
        b += layer.albedo[i + 2];
        n += 1;
    }
    Rgb c = n > 0 ? Rgb{static_cast<float>(r / n / 255.0), static_cast<float>(g / n / 255.0), static_cast<float>(b / n / 255.0)}
                  : Rgb{0.8f, 0.8f, 0.8f};
    cache[name] = c;
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "uso: cramion_dimension_preview <dimension> <salida.obj> [cx cz chunks y_corte semilla]\n");
        return 2;
    }
    const std::string dim = argv[1];
    VoxelGenerator profile = VoxelGenerator::Adventure;
    if (dim == "abismo") profile = VoxelGenerator::Abyss;
    else if (dim == "cielo") profile = VoxelGenerator::Sky;
    else if (dim == "cristal") profile = VoxelGenerator::Crystal;
    const int cx0 = argc > 3 ? std::atoi(argv[3]) : -4;
    const int cz0 = argc > 4 ? std::atoi(argv[4]) : -4;
    const int n = argc > 5 ? std::atoi(argv[5]) : 8;
    const int y_cut = argc > 6 ? std::atoi(argv[6]) : kWorldHeight;
    const int seed = argc > 7 ? std::atoi(argv[7]) : 20260925;
    const Generator gen(seed, 62, profile);

    // La zona con un chunk de borde (para saber que caras se ven).
    const int side = n + 2;
    std::vector<ChunkData> chunks(static_cast<std::size_t>(side * side));
    const auto t0 = std::chrono::steady_clock::now();
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            ChunkData& c = chunks[static_cast<std::size_t>(x + z * side)];
            c.cx = cx0 - 1 + x;
            c.cz = cz0 - 1 + z;
            gen.generate(c);
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%d chunks en %.0f ms (%.2f ms por chunk)\n", side * side, ms, ms / (side * side));

    const int wx0 = (cx0 - 1) * kChunkSize, wz0 = (cz0 - 1) * kChunkSize;
    const auto chunk_of = [&](int wx, int wz) -> const ChunkData* {
        const int x = (wx - wx0) / kChunkSize, z = (wz - wz0) / kChunkSize;
        if (wx < wx0 || wz < wz0 || x >= side || z >= side) return nullptr;
        return &chunks[static_cast<std::size_t>(x + z * side)];
    };
    const auto get = [&](int wx, int y, int wz) -> BlockId {
        if (y < 0) return block::Bedrock;
        if (y >= kWorldHeight || y >= y_cut) return block::Air;
        const ChunkData* c = chunk_of(wx, wz);
        if (c == nullptr) return block::Air;
        return c->get(wx - c->cx * kChunkSize, y, wz - c->cz * kChunkSize);
    };

    std::string solid_v, solid_f, glow_v, glow_f;
    solid_v.reserve(1 << 26);
    solid_f.reserve(1 << 24);
    long long solid_count = 0, glow_count = 0;
    char line[160];
    const auto quad = [&](bool glow, const float p[4][3], Rgb c) {
        std::string& vs = glow ? glow_v : solid_v;
        std::string& fs = glow ? glow_f : solid_f;
        long long& count = glow ? glow_count : solid_count;
        for (int i = 0; i < 4; ++i) {
            std::snprintf(line, sizeof(line), "v %.1f %.1f %.1f %.3f %.3f %.3f\n", static_cast<double>(p[i][0]), static_cast<double>(p[i][1]),
                          static_cast<double>(p[i][2]), static_cast<double>(c.r), static_cast<double>(c.g), static_cast<double>(c.b));
            vs += line;
        }
        std::snprintf(line, sizeof(line), "f %lld %lld %lld %lld\n", count + 1, count + 2, count + 3, count + 4);
        fs += line;
        count += 4;
    };

    const int x_begin = cx0 * kChunkSize, z_begin = cz0 * kChunkSize, x_end = x_begin + n * kChunkSize, z_end = z_begin + n * kChunkSize;
    for (int wz = z_begin; wz < z_end; ++wz) {
        for (int wx = x_begin; wx < x_end; ++wx) {
            const ChunkData* c = chunk_of(wx, wz);
            const std::size_t column = static_cast<std::size_t>((wx - c->cx * kChunkSize) + (wz - c->cz * kChunkSize) * kChunkSize);
            for (int y = 0; y < std::min(y_cut, kWorldHeight); ++y) {
                const BlockId id = get(wx, y, wz);
                const BlockDef& def = blockDef(id);
                if (def.shape == BlockShape::Air || def.shape == BlockShape::Liquid) continue;
                const bool glow = def.light > 0;
                // Variacion por bloque (como voxel.frag en color liso).
                std::uint32_t h = static_cast<std::uint32_t>(wx * 73856093) ^ static_cast<std::uint32_t>(y * 19349663) ^
                                  static_cast<std::uint32_t>(wz * 83492791);
                h ^= h >> 13;
                h *= 0x5bd1e995u;
                h ^= h >> 15;
                const float shade = 0.9f + static_cast<float>(h & 1023u) / 1023.0f * 0.17f;
                const auto tinted = [&](Rgb col, bool top) {
                    std::uint16_t t = 0xFFF;
                    if (def.tint == BlockTint::Grass && (top || def.shape == BlockShape::Cross)) t = c->grass_tint[column];
                    if (def.tint == BlockTint::Foliage) t = c->foliage_tint[column];
                    const float tr = static_cast<float>(t & 15) / 15.0f, tg = static_cast<float>((t >> 4) & 15) / 15.0f,
                                tb = static_cast<float>((t >> 8) & 15) / 15.0f;
                    return Rgb{col.r * tr * shade, col.g * tg * shade, col.b * tb * shade};
                };
                const float x = static_cast<float>(wx), fy = static_cast<float>(y), z = static_cast<float>(wz);
                if (def.shape == BlockShape::Cross) {
                    const Rgb col = tinted(flatColor(id, 0), false);
                    const float a[4][3] = {{x + 0.1f, fy, z + 0.1f}, {x + 0.9f, fy, z + 0.9f}, {x + 0.9f, fy + 0.8f, z + 0.9f}, {x + 0.1f, fy + 0.8f, z + 0.1f}};
                    const float b[4][3] = {{x + 0.9f, fy, z + 0.1f}, {x + 0.1f, fy, z + 0.9f}, {x + 0.1f, fy + 0.8f, z + 0.9f}, {x + 0.9f, fy + 0.8f, z + 0.1f}};
                    quad(glow, a, col);
                    quad(glow, b, col);
                    continue;
                }
                for (int f = 0; f < 6; ++f) {
                    const FaceAxes& ax = kFaces[static_cast<std::size_t>(f)];
                    const BlockId nb = get(wx + ax.n[0], y + ax.n[1], wz + ax.n[2]);
                    if (blockDef(nb).opaque || (nb == id && def.cutout)) continue;
                    const Rgb col = tinted(flatColor(id, f), f == 2);
                    // Esquinas: centro de la cara +- u/2 +- v/2 (antihorario visto desde fuera).
                    const float cx = x + 0.5f + ax.n[0] * 0.5f, cy = fy + 0.5f + ax.n[1] * 0.5f, cz = z + 0.5f + ax.n[2] * 0.5f;
                    static constexpr int kCorner[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
                    float p[4][3];
                    for (int k = 0; k < 4; ++k) {
                        const float su = kCorner[k][0] * 0.5f, sv = kCorner[k][1] * 0.5f;
                        p[k][0] = cx + ax.u[0] * su + ax.v[0] * sv;
                        p[k][1] = cy + ax.u[1] * su + ax.v[1] * sv;
                        p[k][2] = cz + ax.u[2] * su + ax.v[2] * sv;
                    }
                    quad(glow, p, col);
                }
            }
        }
    }
    std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
    if (!out) return 1;
    // OBJ: y arriba; Blender lo gira al importar (eje Y arriba).
    out << "o solid\n" << solid_v;
    out << glow_v;  // vertices de glow despues (los indices de sus caras se desplazan)
    out << solid_f;
    out << "o glow\n";
    // Re-indexar las caras de glow (sus vertices van tras los solidos).
    std::size_t pos = 0;
    while (pos < glow_f.size()) {
        const std::size_t end = glow_f.find('\n', pos);
        const char* p = glow_f.c_str() + pos + 1;  // tras la "f"
        char* next = nullptr;
        out << 'f';
        for (int k = 0; k < 4; ++k) {
            const long long index = std::strtoll(p, &next, 10);
            out << ' ' << index + solid_count;
            p = next;
        }
        out << '\n';
        pos = end + 1;
    }
    std::printf("%lld caras solidas, %lld que brillan -> %s\n", solid_count / 4, glow_count / 4, argv[2]);
    return 0;
}
