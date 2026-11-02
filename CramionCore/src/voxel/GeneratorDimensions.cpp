// Generadores de las dimensiones (VoxelSystem::setDimension), pensados para el
// estilo de color liso (siluetas grandes y colores planos, como Cube World).
// Igual que los otros, son funciones puras de (semilla, x, z): cada chunk se
// genera en cualquier hilo y en cualquier orden, sin cortes entre chunks (lo
// que se sale de su celda, hongos, arboles, cristales y ruinas, se prueba
// desde los chunks vecinos y se escribe la parte que cae dentro).
//
//   abismo   cavernas enormes: suelo de basalto y ceniza con mesetas, mares
//            de lava (bloque que da luz) con islas y orillas de obsidiana,
//            rios de lava, un techo de roca con estalactitas y grandes
//            agujeros al cielo, columnas gigantes, arcos de roca, hongos de
//            fuego y fortalezas en ruinas de ladrillo del abismo.
//   cielo    islas flotantes de varios tamanos (conos invertidos con la
//            panza irregular) sobre el vacio y un mar de nubes, arroyos que
//            caen al vacio en cascadas, arboles dorados, flores y cristales.
//   cristal  colinas de musgo brillante con lagos (el oceano del motor),
//            racimos de cristales de colores, monolitos de cristal y hongos
//            gigantes luminosos; pensado para la noche eterna.

#include "VoxelInternal.h"

#include <algorithm>
#include <cmath>

namespace cramion::voxel {

namespace {

constexpr float kPi = 3.14159265f;

std::uint32_t hash3(int x, int y, int z, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^
                      static_cast<std::uint32_t>(z) * 83492791u ^ seed * 2654435761u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}
float rand01(int x, int y, int z, std::uint32_t seed) {
    return static_cast<float>(hash3(x, y, z, seed) & 0xFFFFFF) / 16777215.0f;
}
float lerp(float a, float b, float t) { return a + (b - a) * t; }
float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
std::uint16_t packTint(float r, float g, float b) {
    const auto c = [](float v) { return static_cast<std::uint16_t>(std::clamp(static_cast<int>(std::lround(v * 15.0f)), 0, 15)); };
    return static_cast<std::uint16_t>(c(r) | (c(g) << 4) | (c(b) << 8));
}
int floorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }

// Escribe en el chunk lo que cae dentro (coordenadas del mundo).
struct Writer {
    ChunkData& chunk;
    int x0 = 0, z0 = 0;
    bool inside(int wx, int y, int wz) const {
        const int lx = wx - x0, lz = wz - z0;
        return lx >= 0 && lz >= 0 && lx < kChunkSize && lz < kChunkSize && y >= 1 && y < kWorldHeight;
    }
    BlockId get(int wx, int y, int wz) const {
        if (!inside(wx, y, wz)) return block::Air;
        return chunk.get(wx - x0, y, wz - z0);
    }
    void put(int wx, int y, int wz, BlockId id, bool only_air) {
        if (!inside(wx, y, wz)) return;
        BlockId& cell = chunk.blocks[static_cast<std::size_t>(cellIndex(wx - x0, y, wz - z0))];
        if (only_air && cell != block::Air && !blockDef(cell).replaceable) return;
        cell = id;
    }
    void set(int lx, int y, int lz, BlockId id) { chunk.blocks[static_cast<std::size_t>(cellIndex(lx, y, lz))] = id; }
};

// Densidad en una rejilla de 4 x 4 x 4 interpolada (16 veces menos ruido).
struct Grid3 {
    static constexpr int kStep = 4;
    static constexpr int kG = kChunkSize / kStep + 1;
    int gy = 0;
    std::vector<float> v;
    template <typename F>
    void build(int x0, int z0, int y_max, F&& f) {
        gy = y_max / kStep + 2;
        v.resize(static_cast<std::size_t>(kG * kG * gy));
        for (int iy = 0; iy < gy; ++iy) {
            for (int iz = 0; iz < kG; ++iz) {
                for (int ix = 0; ix < kG; ++ix) {
                    v[static_cast<std::size_t>(ix + iz * kG + iy * kG * kG)] =
                        f(static_cast<float>(x0 + ix * kStep), static_cast<float>(iy * kStep), static_cast<float>(z0 + iz * kStep));
                }
            }
        }
    }
    float at(int x, int y, int z) const {
        const int ix = x / kStep, iy = y / kStep, iz = z / kStep;
        const float fx = static_cast<float>(x % kStep) / kStep, fy = static_cast<float>(y % kStep) / kStep,
                    fz = static_cast<float>(z % kStep) / kStep;
        const auto G = [&](int a, int b, int c) { return v[static_cast<std::size_t>(a + c * kG + b * kG * kG)]; };
        const float c00 = lerp(G(ix, iy, iz), G(ix + 1, iy, iz), fx);
        const float c10 = lerp(G(ix, iy, iz + 1), G(ix + 1, iy, iz + 1), fx);
        const float c01 = lerp(G(ix, iy + 1, iz), G(ix + 1, iy + 1, iz), fx);
        const float c11 = lerp(G(ix, iy + 1, iz + 1), G(ix + 1, iy + 1, iz + 1), fx);
        return lerp(lerp(c00, c10, fz), lerp(c01, c11, fz), fy);
    }
};

// Punta inclinada (cristales): de `base` en la direccion `dir` (unitaria),
// con radio que va de r0 a casi nada.
void spire(Writer& w, float bx, float by, float bz, float dx, float dy, float dz, float length, float r0, BlockId id) {
    for (float s = 0.0f; s <= length; s += 0.5f) {
        const float k = s / length;
        const float radius = r0 * (1.0f - k) + 0.45f;
        const float px = bx + dx * s, py = by + dy * s, pz = bz + dz * s;
        const int ir = static_cast<int>(std::ceil(radius));
        for (int oy = -ir; oy <= ir; ++oy) {
            for (int oz = -ir; oz <= ir; ++oz) {
                for (int ox = -ir; ox <= ir; ++ox) {
                    if (static_cast<float>(ox * ox + oy * oy + oz * oz) > radius * radius) continue;
                    w.put(static_cast<int>(std::floor(px)) + ox, static_cast<int>(std::floor(py)) + oy,
                          static_cast<int>(std::floor(pz)) + oz, id, false);
                }
            }
        }
    }
}

// Copa redonda (arboles dorados): elipsoide con el borde a medias.
void canopy(Writer& w, int cx, int cy, int cz, float radius, float radius_y, BlockId leaves, std::uint32_t seed) {
    const int ri = static_cast<int>(std::ceil(radius));
    const int ry = static_cast<int>(std::ceil(radius_y));
    for (int dy = -ry; dy <= ry; ++dy) {
        for (int dz = -ri; dz <= ri; ++dz) {
            for (int dx = -ri; dx <= ri; ++dx) {
                const float d = static_cast<float>(dx * dx + dz * dz) / (radius * radius) +
                                static_cast<float>(dy * dy) / (radius_y * radius_y);
                if (d > 1.0f) continue;
                if (d > 0.7f && rand01(cx + dx, cy + dy, cz + dz, seed) < 0.35f) continue;
                w.put(cx + dx, cy + dy, cz + dz, leaves, true);
            }
        }
    }
}

}  // namespace

// =====================================================================================
// Abismo
// =====================================================================================

struct Generator::AbyssColumn {
    int floor = 30;              // ultimo bloque del suelo
    int ceiling = kWorldHeight;  // primer bloque del techo (kWorldHeight = agujero al cielo)
    int ceiling_top = 176;       // ultimo bloque del techo (por fuera)
    float river = 0.0f;          // rio de lava (0..1)
    bool ash = false;            // llanura de ceniza
    float pillar_d = 1e9f;       // distancia al eje de la columna mas cercana
    float pillar_r = 0.0f;       // su radio (0 = ninguna)
    int pillar_top = 0;
};

Generator::AbyssColumn Generator::abyssColumn(int x, int z) const {
    const float fx = static_cast<float>(x), fz = static_cast<float>(z);
    AbyssColumn c;
    // Suelo: llanuras bajas (mares de lava) y mesetas escalonadas.
    float h = 42.0f + fbm2(fx / 240.0f, fz / 240.0f, 4, 301) * 50.0f + fbm2(fx / 45.0f, fz / 45.0f, 2, 302) * 5.0f;
    const float mesa_noise = fbm2(fx / 170.0f, fz / 170.0f, 3, 303);
    const float m = smoothstep(0.06f, 0.18f, mesa_noise);
    if (m > 0.0f) {
        float top = 62.0f + fbm2(fx / 80.0f, fz / 80.0f, 2, 304) * 18.0f;
        top = std::floor(top / 6.0f) * 6.0f;
        h = lerp(h, std::max(h, top), m);
    }
    c.floor = std::clamp(static_cast<int>(std::lround(h)), 4, 120);
    if (c.floor > kAbyssLava + 2) {
        const float rv = std::abs(fbm2(fx / 320.0f, fz / 320.0f, 3, 305) + fbm2(fx / 70.0f, fz / 70.0f, 2, 306) * 0.06f);
        c.river = 1.0f - smoothstep(0.012f, 0.03f, rv);
    }
    c.ash = fbm2(fx / 55.0f, fz / 55.0f, 2, 312) > -0.05f;

    // Techo: bovedas con estalactitas y agujeros al cielo (en embudo).
    float ceiling = 150.0f + fbm2(fx / 130.0f, fz / 130.0f, 3, 307) * 14.0f;
    const float st = std::max(0.0f, fbm2(fx / 11.0f, fz / 11.0f, 2, 308));
    ceiling -= st * st * 80.0f;
    const float hole = fbm2(fx / 280.0f, fz / 280.0f, 3, 309) + fbm2(fx / 60.0f, fz / 60.0f, 2, 310) * 0.2f;
    ceiling += smoothstep(0.0f, 0.12f, hole) * 160.0f;
    c.ceiling_top = 176 + static_cast<int>(std::lround(fbm2(fx / 50.0f, fz / 50.0f, 2, 311) * 6.0f));
    c.ceiling = ceiling >= static_cast<float>(c.ceiling_top - 1)
                    ? kWorldHeight
                    : std::clamp(static_cast<int>(std::lround(ceiling)), c.floor + 8, c.ceiling_top);

    // Columnas gigantes (o agujas, donde no hay techo).
    const auto seed = static_cast<std::uint32_t>(seed_) + 313u;
    constexpr int kCell = 56;
    const int gx = floorDiv(x, kCell), gz = floorDiv(z, kCell);
    for (int oz = -1; oz <= 1; ++oz) {
        for (int ox = -1; ox <= 1; ++ox) {
            const int cx = gx + ox, cz = gz + oz;
            if (rand01(cx, 0, cz, seed) > 0.4f) continue;
            const float px = static_cast<float>(cx * kCell) + 8.0f + rand01(cx, 1, cz, seed) * 40.0f;
            const float pz = static_cast<float>(cz * kCell) + 8.0f + rand01(cx, 2, cz, seed) * 40.0f;
            const float r = 3.0f + rand01(cx, 3, cz, seed) * rand01(cx, 4, cz, seed) * 8.0f;
            // Borde irregular (no un cilindro perfecto).
            const float ang = std::atan2(fz - pz, fx - px);
            const float d = std::hypot(fx - px, fz - pz) * (1.0f + 0.22f * std::sin(ang * 3.0f + rand01(cx, 6, cz, seed) * 6.0f) + fbm2(fx / 6.0f, fz / 6.0f, 1, 314) * 0.2f);
            if (d / r < c.pillar_d / std::max(c.pillar_r, 1e-3f)) {
                c.pillar_d = d;
                c.pillar_r = r;
                c.pillar_top = c.floor + 45 + static_cast<int>(rand01(cx, 5, cz, seed) * 90.0f);
            }
        }
    }
    return c;
}

void Generator::generateAbyss(ChunkData& chunk) const {
    const int x0 = chunk.cx * kChunkSize, z0 = chunk.cz * kChunkSize;
    const auto seed = static_cast<std::uint32_t>(seed_) + 331u;
    Writer w{chunk, x0, z0};
    std::array<AbyssColumn, kChunkSize * kChunkSize> cols{};
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) cols[static_cast<std::size_t>(x + z * kChunkSize)] = abyssColumn(x0 + x, z0 + z);
    }
    chunk.grass_tint.fill(0xFFF);
    chunk.foliage_tint.fill(0xFFF);

    // Arcos y rocas colgadas entre el suelo y el techo.
    constexpr int kRockTop = 168;
    Grid3 rock;
    rock.build(x0, z0, kRockTop, [&](float wx, float wy, float wz) {
        return noise3(wx / 44.0f, wy / 34.0f, wz / 44.0f, 321) - 0.47f;
    });

    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            const AbyssColumn& c = cols[static_cast<std::size_t>(x + z * kChunkSize)];
            const int wx = x0 + x, wz = z0 + z;
            const int y_end = std::min(kWorldHeight - 1, std::max({c.ceiling_top, c.floor, kAbyssLava}));
            const bool low = c.floor <= kAbyssLava + 1;  // orilla o fondo del mar de lava
            const int roof = std::min(c.ceiling, kWorldHeight);
            for (int y = 0; y <= y_end; ++y) {
                BlockId id = block::Air;
                bool solid = y <= c.floor || (c.ceiling < kWorldHeight && y >= c.ceiling && y <= c.ceiling_top);
                if (!solid && c.pillar_r > 0.0f && y < std::min(c.pillar_top, roof) + 3) {
                    // Mas anchas abajo y arriba (se funden con el suelo y el techo).
                    const float a = std::max(0.0f, 1.0f - static_cast<float>(y - c.floor) / 14.0f);
                    const float b = c.ceiling < kWorldHeight ? std::max(0.0f, 1.0f - static_cast<float>(c.ceiling - y) / 14.0f) : 0.0f;
                    const float taper = c.ceiling < kWorldHeight ? 1.0f : 1.0f - 0.7f * smoothstep(static_cast<float>(c.floor), static_cast<float>(c.pillar_top), static_cast<float>(y));
                    solid = c.pillar_d <= c.pillar_r * (taper + 1.3f * a * a + 1.3f * b * b);
                }
                if (!solid && y > c.floor + 8 && y < std::min(roof, kRockTop) - 6) solid = rock.at(x, y, z) > 0.0f;
                if (y == 0 || (y < 3 && rand01(wx, y, wz, seed) < 0.5f)) {
                    id = block::Bedrock;
                } else if (solid) {
                    if (y <= c.floor) {
                        const int depth = c.floor - y;
                        if (depth == 0) {
                            id = low ? (fbm2(static_cast<float>(wx) / 9.0f, static_cast<float>(wz) / 9.0f, 1, 315) > 0.22f ? block::Magma : block::Obsidian)
                                     : (c.ash ? block::Ash : block::Basalt);
                        } else if (depth < 3 && c.ash && !low) {
                            id = block::Ash;
                        } else {
                            id = rand01(wx, y, wz, seed + 1u) < 0.012f ? block::Magma : block::Basalt;
                        }
                    } else if (c.ceiling < kWorldHeight && y == c.ceiling_top) {
                        id = block::Ash;  // por fuera, el techo es un paramo de ceniza
                    } else {
                        id = rand01(wx, y, wz, seed + 2u) < 0.01f ? block::Magma
                                                                   : (rand01(wx, y >> 3, wz, seed + 3u) < 0.06f ? block::Obsidian : block::Basalt);
                    }
                } else if (y <= kAbyssLava && y > c.floor) {
                    id = block::Lava;
                }
                w.set(x, y, z, id);
            }
            // Rios de lava a ras del suelo, con la orilla de magma.
            if (c.river > 0.15f && !low && w.chunk.get(x, c.floor + 1, z) == block::Air) {
                if (c.river > 0.4f) {
                    const int depth = 1 + (c.river > 0.7f ? 1 : 0);
                    for (int k = 0; k < depth; ++k) w.set(x, c.floor - k, z, block::Lava);
                } else {
                    w.set(x, c.floor, z, block::Magma);
                }
            }
            // Hierba de brasas sobre la ceniza.
            const int top = c.floor;
            if (top + 1 < kWorldHeight && w.chunk.get(x, top, z) == block::Ash && w.chunk.get(x, top + 1, z) == block::Air &&
                rand01(wx, 7, wz, seed) < 0.06f) {
                w.set(x, top + 1, z, block::EmberGrass);
            }
            // Ceniza fuera, sobre el techo.
            if (c.ceiling < kWorldHeight && c.ceiling_top + 1 < kWorldHeight && rand01(wx, 8, wz, seed) < 0.05f) {
                w.set(x, c.ceiling_top + 1, z, block::EmberGrass);
            }
        }
    }

    // --- Hongos de fuego (celdas de 10 x 10) ---
    {
        constexpr int kCell = 10, kReach = 8;
        const std::uint32_t s = seed + 50u;
        for (int gz = floorDiv(z0 - kReach, kCell); gz <= floorDiv(z0 + kChunkSize + kReach, kCell); ++gz) {
            for (int gx = floorDiv(x0 - kReach, kCell); gx <= floorDiv(x0 + kChunkSize + kReach, kCell); ++gx) {
                if (rand01(gx, 0, gz, s) > 0.32f) continue;
                const int tx = gx * kCell + static_cast<int>(rand01(gx, 1, gz, s) * kCell);
                const int tz = gz * kCell + static_cast<int>(rand01(gx, 2, gz, s) * kCell);
                const AbyssColumn c = abyssColumn(tx, tz);
                if (c.floor <= kAbyssLava + 2 || c.river > 0.15f || c.pillar_d < c.pillar_r + 3.0f) continue;
                const bool giant = rand01(gx, 3, gz, s) < 0.14f;
                const int h = giant ? 12 + static_cast<int>(rand01(gx, 4, gz, s) * 9.0f) : 4 + static_cast<int>(rand01(gx, 4, gz, s) * 7.0f);
                const float radius = giant ? 5.0f + rand01(gx, 5, gz, s) * 2.5f : 2.2f + rand01(gx, 5, gz, s) * 2.0f;
                const float cap_h = std::max(2.0f, radius * 0.55f);
                const int base = c.floor + 1;
                if (c.ceiling < kWorldHeight && base + h + static_cast<int>(cap_h) + 2 >= c.ceiling) continue;
                for (int i = 0; i < h; ++i) {
                    w.put(tx, base + i, tz, block::FireStem, false);
                    if (giant && i < h - 2) {
                        w.put(tx + 1, base + i, tz, block::FireStem, false);
                        w.put(tx, base + i, tz + 1, block::FireStem, false);
                        w.put(tx + 1, base + i, tz + 1, block::FireStem, false);
                    }
                }
                const int cy = base + h;
                const int ri = static_cast<int>(std::ceil(radius));
                for (int dy = -1; dy <= static_cast<int>(std::ceil(cap_h)); ++dy) {
                    for (int dz = -ri; dz <= ri; ++dz) {
                        for (int dx = -ri; dx <= ri; ++dx) {
                            const float r2 = static_cast<float>(dx * dx + dz * dz) / (radius * radius);
                            if (dy < 0) {
                                if (r2 >= 0.6f && r2 <= 1.0f) w.put(tx + dx, cy + dy, tz + dz, block::FireCap, true);  // ala caida
                                continue;
                            }
                            const float e = r2 + static_cast<float>(dy * dy) / (cap_h * cap_h);
                            if (e <= 1.0f) w.put(tx + dx, cy + dy, tz + dz, block::FireCap, true);
                        }
                    }
                }
            }
        }
    }

    // --- Fortalezas en ruinas (una por region de 112 x 112, a veces) ---
    {
        constexpr int kRegion = 112, kHalf = 12;
        const std::uint32_t s = seed + 90u;
        for (int rz = floorDiv(z0 - kRegion, kRegion); rz <= floorDiv(z0 + kChunkSize, kRegion); ++rz) {
            for (int rx = floorDiv(x0 - kRegion, kRegion); rx <= floorDiv(x0 + kChunkSize, kRegion); ++rx) {
                if (rand01(rx, 0, rz, s) > 0.6f) continue;
                const int cx = rx * kRegion + 30 + static_cast<int>(rand01(rx, 1, rz, s) * 52.0f);
                const int cz = rz * kRegion + 30 + static_cast<int>(rand01(rx, 2, rz, s) * 52.0f);
                if (cx + kHalf < x0 || cx - kHalf >= x0 + kChunkSize || cz + kHalf < z0 || cz - kHalf >= z0 + kChunkSize) continue;
                const AbyssColumn c = abyssColumn(cx, cz);
                if (c.floor <= kAbyssLava + 3 || c.river > 0.1f || (c.ceiling < kWorldHeight && c.ceiling - c.floor < 28)) continue;
                const int base = c.floor + 1;
                for (int wz = std::max(z0, cz - kHalf); wz <= std::min(z0 + kChunkSize - 1, cz + kHalf); ++wz) {
                    for (int wx = std::max(x0, cx - kHalf); wx <= std::min(x0 + kChunkSize - 1, cx + kHalf); ++wx) {
                        const int dx = wx - cx, dz = wz - cz;
                        const int ax = std::abs(dx), az = std::abs(dz), m = std::max(ax, az);
                        // Plataforma con cimientos hasta el suelo.
                        w.put(wx, base, wz, block::AbyssBricks, false);
                        for (int y = base - 1; y > base - 14 && y > 0; --y) {
                            if (blockDef(w.get(wx, y, wz)).solid) break;
                            w.put(wx, y, wz, block::AbyssBricks, false);
                        }
                        // Muros: exterior, torres en las esquinas y torreon central.
                        int wall = 0;
                        const bool tower = ax >= 8 && az >= 8 && (ax == 8 || ax == 12 || az == 8 || az == 12);
                        if (tower) wall = 15;
                        else if (m == 10) wall = 7;
                        else if (m == 3) wall = 11;
                        if ((dz == -10 && ax <= 1) || (dz == -3 && ax == 0)) wall = 0;  // puertas
                        // Ruina: el borde de arriba mordido y trozos caidos.
                        const float erosion = (fbm2(static_cast<float>(wx) / 7.0f, static_cast<float>(wz) / 7.0f, 2, 331) + 0.5f) * 7.0f +
                                              rand01(wx, 9, wz, s) * 3.0f;
                        int height = wall > 0 ? std::max(0, wall - static_cast<int>(erosion)) : 0;
                        if (rand01(wx, 10, wz, s) < 0.08f) height = std::min(height, 1);
                        for (int y = 1; y <= 9; ++y) {  // dentro, limpio
                            if (wall == 0 && m < 12 && w.get(wx, base + y, wz) != block::Lava) w.put(wx, base + y, wz, block::Air, false);
                        }
                        for (int y = 1; y <= height; ++y) w.put(wx, base + y, wz, block::AbyssBricks, false);
                        if (height == wall && wall > 0 && ((dx + dz) & 1) == 0) w.put(wx, base + height + 1, wz, block::AbyssBricks, false);
                        // Braseros de magma y escombros.
                        if (ax == 6 && az == 6) w.put(wx, base + 1, wz, block::Magma, false);
                        else if (wall == 0 && m < 10 && rand01(wx, 11, wz, s) < 0.05f) w.put(wx, base + 1, wz, block::AbyssBricks, false);
                    }
                }
            }
        }
    }
}

// =====================================================================================
// Cielo
// =====================================================================================

struct Generator::SkyIsland {
    float cx = 0.0f, cz = 0.0f;
    float r = 10.0f;      // radio medio
    float top = 100.0f;   // altura de la cima
    float depth = 10.0f;  // de la cima a la punta de abajo
    std::uint32_t id = 0;
    bool big = false;
    bool waterfall = false;
    float fall_angle = 0.0f;
};

namespace {
constexpr int kSkyBigCell = 120;
constexpr int kSkySmallCell = 44;
constexpr int kCloudBase = 22;
}  // namespace

bool Generator::skyIsland(int gx, int gz, bool big, SkyIsland& out) const {
    const std::uint32_t s = static_cast<std::uint32_t>(seed_) + (big ? 601u : 602u);
    if (rand01(gx, 0, gz, s) >= (big ? 0.85f : 0.68f)) return false;
    const float cell = static_cast<float>(big ? kSkyBigCell : kSkySmallCell);
    out.big = big;
    out.id = hash3(gx, 7, gz, s);
    const float k = rand01(gx, 1, gz, s);
    out.r = big ? 14.0f + std::pow(k, 1.6f) * 40.0f : 3.5f + k * 6.0f;
    out.cx = (static_cast<float>(gx) + 0.5f + (rand01(gx, 2, gz, s) - 0.5f) * 0.45f) * cell;
    out.cz = (static_cast<float>(gz) + 0.5f + (rand01(gx, 3, gz, s) - 0.5f) * 0.45f) * cell;
    out.top = big ? 82.0f + rand01(gx, 4, gz, s) * 78.0f : 70.0f + rand01(gx, 4, gz, s) * 110.0f;
    out.depth = out.r * (0.9f + rand01(gx, 5, gz, s) * 0.7f) + (big ? 6.0f : 2.0f);
    out.waterfall = big && out.r > 13.0f && rand01(gx, 6, gz, s) < 0.9f;
    out.fall_angle = rand01(gx, 8, gz, s) * 2.0f * kPi;
    return true;
}

bool Generator::skySpan(const SkyIsland& isl, int x, int z, int& bottom, int& top, float& t) const {
    const float fx = static_cast<float>(x), fz = static_cast<float>(z);
    const float d = std::hypot(fx - isl.cx, fz - isl.cz);
    if (d > isl.r * 1.3f) return false;
    // Costa irregular.
    const float scale = isl.r * 0.7f + 6.0f;
    const float re = isl.r * (0.82f + 0.36f * (fbm2(fx / scale, fz / scale, 2, 610 + static_cast<int>(isl.id % 7u)) + 0.5f));
    t = d / re;
    if (t >= 1.0f) return false;
    const float bumps = fbm2(fx / 22.0f, fz / 22.0f, 2, 620) * std::min(isl.r * 0.18f, 6.0f);
    const float top_f = isl.top + (1.0f - t * t) * isl.r * 0.10f + bumps - smoothstep(0.75f, 1.0f, t) * 2.0f;
    const float drips = std::max(0.0f, fbm2(fx / 6.0f, fz / 6.0f, 2, 621)) * 9.0f * (1.0f - t);
    const float bottom_f = isl.top - 2.0f - isl.depth * std::pow(1.0f - t, 0.55f) - drips;
    top = std::clamp(static_cast<int>(std::lround(top_f)), 2, kWorldHeight - 30);
    bottom = std::clamp(std::min(static_cast<int>(std::lround(bottom_f)), top - 1), 1, top);
    return true;
}

int Generator::skyTop(int x, int z, float* t_out) const {
    int best = -1;
    float best_t = 1.0f;
    const auto scan = [&](bool big) {
        const int cell = big ? kSkyBigCell : kSkySmallCell;
        const int gx = floorDiv(x, cell), gz = floorDiv(z, cell);
        for (int oz = -1; oz <= 1; ++oz) {
            for (int ox = -1; ox <= 1; ++ox) {
                SkyIsland isl;
                if (!skyIsland(gx + ox, gz + oz, big, isl)) continue;
                int b = 0, tp = 0;
                float t = 0.0f;
                if (skySpan(isl, x, z, b, tp, t) && tp > best) {
                    best = tp;
                    best_t = t;
                }
            }
        }
    };
    scan(true);
    scan(false);
    if (t_out != nullptr) *t_out = best_t;
    return best;
}

void Generator::generateSky(ChunkData& chunk) const {
    const int x0 = chunk.cx * kChunkSize, z0 = chunk.cz * kChunkSize;
    const auto seed = static_cast<std::uint32_t>(seed_) + 641u;
    Writer w{chunk, x0, z0};
    chunk.grass_tint.fill(packTint(0.70f, 1.0f, 0.92f));
    chunk.foliage_tint.fill(0xFFF);

    // Islas que pueden tocar el chunk.
    std::vector<SkyIsland> islands;
    const auto gather = [&](bool big) {
        const int cell = big ? kSkyBigCell : kSkySmallCell;
        const int reach = big ? 100 : 40;
        for (int gz = floorDiv(z0 - reach, cell); gz <= floorDiv(z0 + kChunkSize + reach, cell); ++gz) {
            for (int gx = floorDiv(x0 - reach, cell); gx <= floorDiv(x0 + kChunkSize + reach, cell); ++gx) {
                SkyIsland isl;
                if (!skyIsland(gx, gz, big, isl)) continue;
                const float m = isl.r * 1.3f + 4.0f;
                if (isl.cx + m < static_cast<float>(x0) || isl.cx - m > static_cast<float>(x0 + kChunkSize) ||
                    isl.cz + m < static_cast<float>(z0) || isl.cz - m > static_cast<float>(z0 + kChunkSize)) {
                    continue;
                }
                islands.push_back(isl);
            }
        }
    };
    gather(true);
    gather(false);

    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            const int wx = x0 + x, wz = z0 + z;
            const float fx = static_cast<float>(wx), fz = static_cast<float>(wz);
            // Mar de nubes (con claros que dejan ver el vacio).
            // Cumulos redondos (ruido elevado a una potencia) que se adelgazan
            // en el borde de los claros.
            const float sea_mask = fbm2(fx / 170.0f, fz / 170.0f, 2, 632) + 0.18f;
            if (sea_mask > 0.0f) {
                const float edge = std::min(1.0f, sea_mask * 6.0f);
                const float billow = std::pow(std::max(0.0f, fbm2(fx / 36.0f, fz / 36.0f, 2, 630) + 0.2f), 1.4f) * 26.0f;
                const float puff = std::max(0.0f, fbm2(fx / 11.0f, fz / 11.0f, 2, 631)) * 5.0f;
                const int cloud_top = 27 + static_cast<int>(std::lround((billow + puff) * edge));
                const int cloud_bottom = kCloudBase + static_cast<int>(std::lround(fbm2(fx / 30.0f, fz / 30.0f, 2, 633) * 3.0f + (1.0f - edge) * 5.0f));
                for (int y = cloud_bottom; y <= cloud_top; ++y) w.set(x, y, z, block::Cloud);
            }
            int highest = -1;
            float highest_t = 1.0f;
            for (const SkyIsland& isl : islands) {
                int bottom = 0, top = 0;
                float t = 0.0f;
                if (!skySpan(isl, wx, wz, bottom, top, t)) continue;
                for (int y = bottom; y <= top; ++y) {
                    const int depth = top - y;
                    BlockId id = block::SkyStone;
                    if (depth == 0) id = t < 0.97f ? block::SkyGrass : block::SkySoil;
                    else if (depth <= 3 && y > bottom + 1) id = block::SkySoil;
                    w.set(x, y, z, id);
                }
                // Cristales colgando de la panza.
                if (t < 0.6f && rand01(wx, bottom, wz, seed + 1u) < 0.012f) {
                    const int n = 1 + static_cast<int>(rand01(wx, 2, wz, seed + 1u) * 4.0f);
                    for (int k = 1; k <= n && bottom - k > 0; ++k) w.set(x, bottom - k, z, block::SkyCrystal);
                }
                if (top > highest) {
                    highest = top;
                    highest_t = t;
                }
            }
            // Hierba y flores.
            if (highest > 0 && highest + 1 < kWorldHeight && highest_t < 0.95f && w.chunk.get(x, highest, z) == block::SkyGrass &&
                w.chunk.get(x, highest + 1, z) == block::Air) {
                const float r = rand01(wx, 7, wz, seed);
                const BlockId plant = r < 0.24f ? block::TallGrass : (r < 0.30f ? block::SkyFlower : (r < 0.31f ? block::WhiteFlower : block::Air));
                if (plant != block::Air) w.set(x, highest + 1, z, plant);
            }
        }
    }

    // --- Arroyos que caen al vacio ---
    for (const SkyIsland& isl : islands) {
        if (!isl.waterfall) continue;
        const float ux = std::cos(isl.fall_angle), uz = std::sin(isl.fall_angle);
        // Borde de la isla en esa direccion.
        float rim = 0.0f;
        int rim_top = 0;
        for (float d = isl.r * 0.3f; d < isl.r * 1.4f; d += 1.0f) {
            int b = 0, tp = 0;
            float t = 0.0f;
            if (!skySpan(isl, static_cast<int>(std::floor(isl.cx + ux * d)), static_cast<int>(std::floor(isl.cz + uz * d)), b, tp, t)) break;
            rim = d;
            rim_top = tp;
        }
        if (rim <= 0.0f) continue;
        for (int lane = 0; lane < 3; ++lane) {
            const float off = static_cast<float>(lane) - 1.0f;
            // Cauce: el bloque de arriba es agua.
            for (float d = isl.r * 0.4f; d <= rim; d += 0.5f) {
                const int px = static_cast<int>(std::floor(isl.cx + ux * d - uz * off));
                const int pz = static_cast<int>(std::floor(isl.cz + uz * d + ux * off));
                if (!w.inside(px, 1, pz)) continue;
                int b = 0, tp = 0;
                float t = 0.0f;
                if (!skySpan(isl, px, pz, b, tp, t)) continue;
                w.put(px, tp, pz, block::FallingWater, false);
                w.put(px, tp + 1, pz, block::Air, false);
            }
            // Cascada: del borde hasta las nubes (o lo primero que encuentre).
            for (float d = rim + 1.0f; d <= rim + 2.0f; d += 1.0f) {
                const int px = static_cast<int>(std::floor(isl.cx + ux * d - uz * off));
                const int pz = static_cast<int>(std::floor(isl.cz + uz * d + ux * off));
                if (!w.inside(px, 1, pz)) continue;
                for (int y = rim_top; y > kCloudBase; --y) {
                    const BlockId here = w.get(px, y, pz);
                    if (here != block::Air && here != block::FallingWater && !blockDef(here).replaceable) break;
                    w.put(px, y, pz, block::FallingWater, false);
                }
            }
        }
    }

    // --- Arboles dorados y cristales (celdas pequenas, de los vecinos tambien) ---
    {
        constexpr int kCell = 6, kReach = 6;
        const std::uint32_t s = seed + 70u;
        for (int gz = floorDiv(z0 - kReach, kCell); gz <= floorDiv(z0 + kChunkSize + kReach, kCell); ++gz) {
            for (int gx = floorDiv(x0 - kReach, kCell); gx <= floorDiv(x0 + kChunkSize + kReach, kCell); ++gx) {
                const float roll = rand01(gx, 0, gz, s);
                if (roll > 0.34f) continue;
                const int tx = gx * kCell + static_cast<int>(rand01(gx, 1, gz, s) * kCell);
                const int tz = gz * kCell + static_cast<int>(rand01(gx, 2, gz, s) * kCell);
                float t = 1.0f;
                const int ground = skyTop(tx, tz, &t);
                if (ground < 0 || t > 0.8f) continue;
                const int base = ground + 1;
                if (roll < 0.04f) {
                    // Racimo de cristales del cielo.
                    const int n = 2 + static_cast<int>(rand01(gx, 3, gz, s) * 3.0f);
                    for (int i = 0; i < n; ++i) {
                        const float dx = (rand01(gx, 10 + i, gz, s) - 0.5f) * 0.9f, dz = (rand01(gx, 20 + i, gz, s) - 0.5f) * 0.9f;
                        const float len = std::sqrt(dx * dx + dz * dz + 1.0f);
                        spire(w, static_cast<float>(tx) + 0.5f, static_cast<float>(base) - 1.0f, static_cast<float>(tz) + 0.5f, dx / len,
                              1.0f / len, dz / len, 3.0f + rand01(gx, 30 + i, gz, s) * 5.0f, 0.9f, block::SkyCrystal);
                    }
                    continue;
                }
                const float size = rand01(gx, 4, gz, s);
                const bool giant = size > 0.93f;
                const int h = giant ? 9 + static_cast<int>(rand01(gx, 5, gz, s) * 4.0f) : 4 + static_cast<int>(size * 4.0f);
                const float radius = giant ? 5.0f : 2.4f + size * 1.4f;
                const int top = base + h;
                canopy(w, tx, top, tz, radius, radius * 0.75f, block::GoldenLeaves, s + 9u);
                // Hojas que cuelgan del borde de abajo.
                const int ri = static_cast<int>(radius);
                for (int dz = -ri; dz <= ri; ++dz) {
                    for (int dx = -ri; dx <= ri; ++dx) {
                        if (dx * dx + dz * dz > ri * ri || rand01(tx + dx, 3, tz + dz, s) > 0.18f) continue;
                        const int len = 1 + static_cast<int>(rand01(tx + dx, 4, tz + dz, s) * 3.0f);
                        const int start = top - static_cast<int>(radius * 0.6f);
                        for (int k = 0; k < len; ++k) w.put(tx + dx, start - k, tz + dz, block::GoldenLeaves, true);
                    }
                }
                for (int i = 0; i < h; ++i) w.put(tx, base + i, tz, block::GoldenLog, false);
                if (giant) {
                    for (int i = 0; i < h - 3; ++i) {
                        w.put(tx + 1, base + i, tz, block::GoldenLog, false);
                        w.put(tx, base + i, tz + 1, block::GoldenLog, false);
                        w.put(tx + 1, base + i, tz + 1, block::GoldenLog, false);
                    }
                }
            }
        }
    }
}

// =====================================================================================
// Cristal
// =====================================================================================

int Generator::crystalHeight(int x, int z) const {
    const float fx = static_cast<float>(x), fz = static_cast<float>(z);
    float h = static_cast<float>(sea_) + 3.0f + fbm2(fx / 230.0f, fz / 230.0f, 4, 701) * 44.0f + fbm2(fx / 60.0f, fz / 60.0f, 2, 702) * 6.0f;
    // Colinas redondas aqui y alla.
    h += std::pow(std::max(0.0f, fbm2(fx / 120.0f, fz / 120.0f, 2, 703) + 0.1f), 2.0f) * 60.0f;
    return std::clamp(static_cast<int>(std::lround(h)), 6, kWorldHeight - 60);
}

int Generator::dimensionHeight(int x, int z) const {
    switch (profile_) {
        case VoxelGenerator::Abyss: return abyssColumn(x, z).floor;
        case VoxelGenerator::Sky: return skyTop(x, z);
        case VoxelGenerator::Crystal: return crystalHeight(x, z);
        default: return sea_;
    }
}

void Generator::generateCrystal(ChunkData& chunk) const {
    const int x0 = chunk.cx * kChunkSize, z0 = chunk.cz * kChunkSize;
    const auto seed = static_cast<std::uint32_t>(seed_) + 731u;
    Writer w{chunk, x0, z0};
    chunk.grass_tint.fill(0xFFF);
    chunk.foliage_tint.fill(0xFFF);
    static constexpr BlockId kCrystals[4] = {block::CrystalPink, block::CrystalBlue, block::CrystalViolet, block::CrystalGreen};

    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            const int wx = x0 + x, wz = z0 + z;
            const int h = crystalHeight(wx, wz);
            const bool dry = h >= sea_;
            for (int y = 0; y <= std::max(h, sea_ - 1); ++y) {
                BlockId id = block::Air;
                if (y == 0 || (y < 4 && rand01(wx, y, wz, seed) < 0.5f)) {
                    id = block::Bedrock;
                } else if (y <= h) {
                    const int depth = h - y;
                    if (depth == 0) {
                        id = dry ? (h <= sea_ ? block::DuskSoil : block::GlowMoss)
                                 : (rand01(wx, 1, wz, seed) < 0.3f ? block::Gravel : block::DuskStone);
                    } else if (depth < 4) {
                        id = dry ? block::DuskSoil : block::DuskStone;
                    } else {
                        const float r = rand01(wx, y, wz, seed + 2u);
                        id = r < 0.004f ? kCrystals[static_cast<int>(r * 1000.0f) & 3] : block::DuskStone;
                    }
                } else if (y < sea_) {
                    id = block::Water;
                }
                w.set(x, y, z, id);
            }
            if (!dry || h + 1 >= kWorldHeight) continue;
            const float r = rand01(wx, 7, wz, seed);
            BlockId plant = block::Air;
            if (r < 0.30f) plant = block::GlowGrass;
            else if (r < 0.312f) plant = block::CrystalSprout;
            else if (r < 0.325f) plant = block::GlowMushroom;
            if (plant != block::Air) w.set(x, h + 1, z, plant);
        }
    }

    const auto color_at = [&](int x, int z, float pick) {
        const float region = (fbm2(static_cast<float>(x) / 150.0f, static_cast<float>(z) / 150.0f, 2, 710) + 0.5f) * 4.0f;
        const int i = pick < 0.3f ? static_cast<int>(pick * 13.0f) : static_cast<int>(region);
        return kCrystals[std::clamp(i, 0, 3) & 3];
    };

    // --- Monolitos de cristal (raros, enormes) ---
    {
        constexpr int kCell = 72, kReach = 22;
        const std::uint32_t s = seed + 40u;
        for (int gz = floorDiv(z0 - kReach, kCell); gz <= floorDiv(z0 + kChunkSize + kReach, kCell); ++gz) {
            for (int gx = floorDiv(x0 - kReach, kCell); gx <= floorDiv(x0 + kChunkSize + kReach, kCell); ++gx) {
                if (rand01(gx, 0, gz, s) > 0.55f) continue;
                const int tx = gx * kCell + 10 + static_cast<int>(rand01(gx, 1, gz, s) * (kCell - 20));
                const int tz = gz * kCell + 10 + static_cast<int>(rand01(gx, 2, gz, s) * (kCell - 20));
                const int ground = crystalHeight(tx, tz);
                if (ground < sea_) continue;
                const BlockId id = color_at(tx, tz, rand01(gx, 3, gz, s));
                const int n = 1 + static_cast<int>(rand01(gx, 4, gz, s) * 3.0f);
                for (int i = 0; i < n; ++i) {
                    const float dx = (rand01(gx, 10 + i, gz, s) - 0.5f) * (i == 0 ? 0.35f : 1.0f);
                    const float dz = (rand01(gx, 20 + i, gz, s) - 0.5f) * (i == 0 ? 0.35f : 1.0f);
                    const float len = std::sqrt(dx * dx + dz * dz + 1.0f);
                    const float length = i == 0 ? 24.0f + rand01(gx, 5, gz, s) * 18.0f : 10.0f + rand01(gx, 30 + i, gz, s) * 10.0f;
                    spire(w, static_cast<float>(tx) + 0.5f, static_cast<float>(ground) - 2.0f, static_cast<float>(tz) + 0.5f, dx / len, 1.0f / len,
                          dz / len, length, i == 0 ? 2.6f + rand01(gx, 6, gz, s) * 1.2f : 1.6f, id);
                }
            }
        }
    }

    // --- Hongos gigantes ---
    {
        constexpr int kCell = 17, kReach = 13;
        const std::uint32_t s = seed + 60u;
        for (int gz = floorDiv(z0 - kReach, kCell); gz <= floorDiv(z0 + kChunkSize + kReach, kCell); ++gz) {
            for (int gx = floorDiv(x0 - kReach, kCell); gx <= floorDiv(x0 + kChunkSize + kReach, kCell); ++gx) {
                if (rand01(gx, 0, gz, s) > 0.3f) continue;
                const int tx = gx * kCell + 2 + static_cast<int>(rand01(gx, 1, gz, s) * (kCell - 4));
                const int tz = gz * kCell + 2 + static_cast<int>(rand01(gx, 2, gz, s) * (kCell - 4));
                const int ground = crystalHeight(tx, tz);
                if (ground < sea_) continue;
                const float size = rand01(gx, 3, gz, s);
                const int h = 7 + static_cast<int>(size * 14.0f);
                const bool thick = h > 13;
                const float bend_x = (rand01(gx, 4, gz, s) - 0.5f) * 5.0f, bend_z = (rand01(gx, 5, gz, s) - 0.5f) * 5.0f;
                const BlockId cap_id = rand01(gx, 6, gz, s) < 0.5f ? block::ShroomCapBlue : block::ShroomCapViolet;
                const int base = ground + 1;
                int sx = tx, sz = tz;
                for (int i = 0; i < h; ++i) {
                    const float k = static_cast<float>(i) / static_cast<float>(h);
                    sx = tx + static_cast<int>(std::lround(bend_x * k * k));
                    sz = tz + static_cast<int>(std::lround(bend_z * k * k));
                    w.put(sx, base + i, sz, block::ShroomStem, false);
                    if (thick) {
                        w.put(sx + 1, base + i, sz, block::ShroomStem, false);
                        w.put(sx, base + i, sz + 1, block::ShroomStem, false);
                        w.put(sx + 1, base + i, sz + 1, block::ShroomStem, false);
                    }
                }
                const float radius = 4.0f + size * 4.0f + rand01(gx, 7, gz, s) * 1.5f;
                const float cap_h = radius * 0.55f;
                const float inner = radius - 1.6f;
                const int cy = base + h;
                const int ri = static_cast<int>(std::ceil(radius));
                for (int dy = -1; dy <= static_cast<int>(std::ceil(cap_h)); ++dy) {
                    for (int dz = -ri; dz <= ri; ++dz) {
                        for (int dx = -ri; dx <= ri; ++dx) {
                            const float r2 = static_cast<float>(dx * dx + dz * dz);
                            if (dy < 0) {
                                const float k = r2 / (radius * radius);
                                if (k >= 0.72f && k <= 1.0f) w.put(sx + dx, cy + dy, sz + dz, cap_id, true);
                                continue;
                            }
                            const float outer_e = r2 / (radius * radius) + static_cast<float>(dy * dy) / (cap_h * cap_h);
                            const float in_y = static_cast<float>(dy) + 1.2f;
                            const float inner_e = r2 / (inner * inner) + in_y * in_y / (cap_h * cap_h);
                            if (outer_e <= 1.0f && inner_e > 1.0f) w.put(sx + dx, cy + dy, sz + dz, cap_id, true);
                        }
                    }
                }
            }
        }
    }

    // --- Racimos de cristales ---
    {
        constexpr int kCell = 8, kReach = 8;
        const std::uint32_t s = seed + 80u;
        for (int gz = floorDiv(z0 - kReach, kCell); gz <= floorDiv(z0 + kChunkSize + kReach, kCell); ++gz) {
            for (int gx = floorDiv(x0 - kReach, kCell); gx <= floorDiv(x0 + kChunkSize + kReach, kCell); ++gx) {
                if (rand01(gx, 0, gz, s) > 0.2f) continue;
                const int tx = gx * kCell + static_cast<int>(rand01(gx, 1, gz, s) * kCell);
                const int tz = gz * kCell + static_cast<int>(rand01(gx, 2, gz, s) * kCell);
                const int ground = crystalHeight(tx, tz);
                if (ground < sea_ - 3) continue;
                const BlockId id = color_at(tx, tz, rand01(gx, 3, gz, s));
                const int n = 1 + static_cast<int>(rand01(gx, 4, gz, s) * 4.0f);
                for (int i = 0; i < n; ++i) {
                    const float dx = (rand01(gx, 10 + i, gz, s) - 0.5f) * (i == 0 ? 0.4f : 1.3f);
                    const float dz = (rand01(gx, 20 + i, gz, s) - 0.5f) * (i == 0 ? 0.4f : 1.3f);
                    const float len = std::sqrt(dx * dx + dz * dz + 1.0f);
                    const float length = (i == 0 ? 6.0f : 3.0f) + rand01(gx, 30 + i, gz, s) * (i == 0 ? 9.0f : 5.0f);
                    spire(w, static_cast<float>(tx) + 0.5f, static_cast<float>(ground), static_cast<float>(tz) + 0.5f, dx / len, 1.0f / len,
                          dz / len, length, i == 0 ? 1.3f : 0.7f, id);
                }
            }
        }
    }
}

}  // namespace cramion::voxel
