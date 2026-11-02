// Generador "Aventura" del mundo de bloques (VoxelWorld::generator = 1): un
// mundo para explorar, pensado para el estilo de color liso. Como el Clasico,
// es una funcion pura de (semilla, x, z): cada chunk se genera en cualquier
// hilo y en cualquier orden, sin cortes entre chunks.
//
//   - Relieve: continentes grandes, colinas, montanas afiladas ("ridged") y
//     detalle fino. Donde la pendiente es fuerte, acantilados de piedra.
//   - Rios: cauces que serpentean (donde un ruido se anula) con su valle; el
//     cauce baja por debajo del nivel del mar y el oceano del motor (WaterBody)
//     lo llena, asi que el agua se ve sin bloques de agua dibujados.
//   - Biomas por temperatura y humedad: pradera (campos de flores), bosque
//     (helechos, arbustos, setas, musgo), desierto (dunas y mesetas de
//     terracota por capas), nieve, montana con nieve en las cumbres, playa
//     (palmeras), rio (juncos) y oceano. Agua helada en el frio.
//   - Cuevas: tuneles anchos y cavernas hondas, con entradas en las colinas;
//     suelo con musgo, setas y setas luminosas (dan luz).
//   - Arboles: robles de copa redonda (algunos enormes), abedules, abetos,
//     cactus con brazos y palmeras.

#include "VoxelInternal.h"
#include "Structures.h"

#include <algorithm>
#include <cmath>

namespace cramion::voxel {

namespace {

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

using AB = Generator::AdventureBiome;

}  // namespace

// --- Columna: altura, clima, rio y bioma -------------------------------------------------

// --- Lagos -------------------------------------------------------------------------------
// Uno como mucho por celda de 160 x 160 m, con el centro a mas de 40 m del borde
// de la celda y radio + orilla < 30 m: cada columna solo mira el de su celda.
// Solo donde el terreno de alrededor es llano y seco; el agua llega hasta el
// borde mas bajo, y el fondo es un cuenco. Fuera del agua, un ribazo que la
// contiene (ninguna columna de al lado queda por debajo del agua).
namespace {
constexpr int kLakeCell = 160;
int floorDivInt(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
}  // namespace

Generator::Lake Generator::adventureLake(int gx, int gz) const {
    const std::int64_t key = (static_cast<std::int64_t>(gx) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(gz));
    // El ultimo de este hilo (las columnas seguidas caen en la misma celda).
    struct Last {
        std::uint64_t cache = 0;  // LakeCache::id (no la direccion: se reutiliza)
        std::int64_t key = 0;
        Lake lake;
    };
    thread_local Last last;
    const std::uint64_t cache_id = lakes_ ? lakes_->id : 0;
    if (cache_id != 0 && last.cache == cache_id && last.key == key) return last.lake;
    if (lakes_) {
        std::lock_guard lock(lakes_->mutex);
        if (const auto it = lakes_->cells.find(key); it != lakes_->cells.end()) {
            last = Last{cache_id, key, it->second};
            return it->second;
        }
    }
    Lake lake;
    const auto s = static_cast<std::uint32_t>(seed_) + 4241u;
    if (rand01(gx, 11, gz, s) < 0.45f) {
        lake.x = static_cast<float>(gx * kLakeCell) + 40.0f + rand01(gx, 12, gz, s) * 80.0f;
        lake.z = static_cast<float>(gz * kLakeCell) + 40.0f + rand01(gx, 13, gz, s) * 80.0f;
        lake.radius = 8.0f + rand01(gx, 14, gz, s) * 14.0f;
        lake.depth = 2.5f + rand01(gx, 15, gz, s) * 4.5f;
        const AdventureColumn center = adventureColumnBase(static_cast<int>(lake.x), static_cast<int>(lake.z));
        bool ok = center.height > sea_ + 3 && center.river < 0.05f;
        int lo = center.height, hi = center.height;
        for (int k = 0; k < 12 && ok; ++k) {
            const float a = static_cast<float>(k) * 0.5235988f;
            const AdventureColumn c = adventureColumnBase(static_cast<int>(std::floor(lake.x + std::cos(a) * (lake.radius + 3.0f))),
                                                          static_cast<int>(std::floor(lake.z + std::sin(a) * (lake.radius + 3.0f))));
            ok = c.height > sea_ + 3 && c.river < 0.05f;
            lo = std::min(lo, c.height);
            hi = std::max(hi, c.height);
        }
        lake.level = lo - 1;
        lake.valid = ok && hi - lo <= 7 && lake.level > sea_ + 2 && lake.level < kWorldHeight - 40;
    }
    if (lakes_) {
        std::lock_guard lock(lakes_->mutex);
        if (lakes_->cells.size() > 50000) lakes_->cells.clear();
        lakes_->cells[key] = lake;
    }
    last = Last{cache_id, key, lake};
    return lake;
}

Generator::AdventureColumn Generator::adventureColumn(int x, int z) const {
    AdventureColumn c = adventureColumnBase(x, z);
    c.water = c.height < sea_ ? sea_ : 0;
    const Lake lake = adventureLake(floorDivInt(x, kLakeCell), floorDivInt(z, kLakeCell));
    if (!lake.valid) return c;
    const float dx = static_cast<float>(x) + 0.5f - lake.x, dz = static_cast<float>(z) + 0.5f - lake.z;
    const float d = std::sqrt(dx * dx + dz * dz);
    if (d >= lake.radius + 7.0f) return c;
    if (d < lake.radius) {
        const float k = d / lake.radius;
        const int floor_y = lake.level - static_cast<int>(std::lround(lake.depth * (1.0f - k * k)));
        if (floor_y < lake.level) {
            c.height = floor_y;
            c.water = lake.level + 1;
            c.biome = AB::ARiver;  // fondo de arena, grava y arcilla; sin arboles
            c.mesa = false;
        } else {
            c.height = lake.level;
        }
    } else {
        // Ribazo: nunca por debajo del agua al lado de ella.
        c.height = std::max(c.height, lake.level - std::max(0, static_cast<int>(std::floor(d - lake.radius - 1.0f))));
    }
    c.lake = d < lake.radius + 2.0f;
    return c;
}

Generator::AdventureColumn Generator::adventureColumnBase(int x, int z) const {
    const float fx = static_cast<float>(x), fz = static_cast<float>(z);
    const float sea = static_cast<float>(sea_);
    AdventureColumn c;

    // Tierra y mar: continentes grandes con costas irregulares.
    const float continent = fbm2(fx / 1300.0f, fz / 1300.0f, 4, 101) + fbm2(fx / 260.0f, fz / 260.0f, 2, 102) * 0.12f + 0.15f;
    const float land = std::clamp(continent * 2.6f, -1.0f, 1.0f);
    const float hills = fbm2(fx / 150.0f, fz / 150.0f, 4, 103);
    const float detail = fbm2(fx / 38.0f, fz / 38.0f, 2, 104);
    const float ridge = 1.0f - std::abs(fbm2(fx / 420.0f, fz / 420.0f, 4, 105));
    const float mountain_mask = std::clamp((fbm2(fx / 800.0f, fz / 800.0f, 3, 106) + 0.02f) * 2.4f, 0.0f, 1.0f);
    const float inland = std::clamp(land * 2.2f, 0.0f, 1.0f);
    float h;
    if (land < 0.0f) {
        h = sea + land * 30.0f + hills * 4.0f;  // fondo marino
    } else {
        h = sea + 2.0f + land * 12.0f + hills * 14.0f * inland + detail * 2.5f * inland +
            std::pow(ridge, 2.6f) * 105.0f * mountain_mask * inland;
    }

    // Clima (la altura enfria).
    c.temperature = fbm2(fx / 1200.0f, fz / 1200.0f, 3, 107) * 1.7f - std::max(0.0f, (h - sea - 45.0f) / 55.0f);
    c.humidity = fbm2(fx / 1000.0f, fz / 1000.0f, 3, 108) * 1.7f;
    const bool hot_dry = land > 0.05f && c.temperature > 0.3f && c.humidity < 0.05f;

    // Mesetas del desierto: escalones de 5 bloques con la cima plana.
    if (hot_dry) {
        const float mesa_noise = fbm2(fx / 220.0f, fz / 220.0f, 3, 109);
        const float m = smoothstep(0.16f, 0.26f, mesa_noise) * std::clamp((c.temperature - 0.3f) * 5.0f, 0.0f, 1.0f);
        if (m > 0.0f) {
            float top = sea + 20.0f + mesa_noise * 40.0f;
            top = std::floor(top / 5.0f) * 5.0f;
            h = lerp(h, std::max(h, top), m);
            c.mesa = m > 0.5f;
        }
    }

    // Rios: donde un ruido de baja frecuencia pasa por cero. Valle ancho y cauce estrecho.
    if (land > 0.0f) {
        const float rv = std::abs(fbm2(fx / 600.0f, fz / 600.0f, 3, 110) + fbm2(fx / 150.0f, fz / 150.0f, 2, 111) * 0.07f);
        const float width = 0.022f + 0.012f * std::clamp(c.humidity * 0.5f + 0.5f, 0.0f, 1.0f);
        c.river = (1.0f - smoothstep(width * 0.6f, width * 1.8f, rv)) * std::clamp(land * 6.0f, 0.0f, 1.0f);
        const float valley = (1.0f - smoothstep(width, width * 7.0f, rv)) * std::clamp(land * 6.0f, 0.0f, 1.0f);
        const float floodplain = sea + 2.0f + (h - sea - 2.0f) * 0.3f;
        h = lerp(h, std::min(h, floodplain), valley * 0.7f);
        h = lerp(h, std::min(h, sea - 3.0f), smoothstep(0.0f, 1.0f, c.river));
    }
    c.height = std::clamp(static_cast<int>(std::lround(h)), 4, kWorldHeight - 30);

    // Bioma.
    if (c.height < sea_ - 1) {
        c.biome = c.river > 0.3f ? AB::ARiver : AB::AOcean;
    } else if (c.river > 0.25f && c.height <= sea_ + 2) {
        c.biome = AB::ARiver;
    } else if (c.height <= sea_ + 2 && land < 0.1f) {
        c.biome = c.temperature < -0.45f ? AB::ASnow : AB::ABeach;
    } else if (c.height > sea_ + 58) {
        c.biome = AB::AMountains;
    } else if (c.temperature < -0.35f) {
        c.biome = AB::ASnow;
    } else if (hot_dry) {
        c.biome = AB::ADesert;
    } else if (c.humidity > 0.1f) {
        c.biome = AB::AForest;
    } else {
        c.biome = AB::AMeadow;
    }
    return c;
}

// --- Chunk ------------------------------------------------------------------------------

void Generator::generateAdventure(ChunkData& chunk) const {
    const int x0 = chunk.cx * kChunkSize;
    const int z0 = chunk.cz * kChunkSize;
    const auto seed = static_cast<std::uint32_t>(seed_);

    // Columnas con un borde de 1 (para la pendiente).
    constexpr int kB = kChunkSize + 2;
    std::array<AdventureColumn, kB * kB> cols{};
    int max_height = sea_;
    for (int z = -1; z <= kChunkSize; ++z) {
        for (int x = -1; x <= kChunkSize; ++x) {
            const AdventureColumn c = adventureColumn(x0 + x, z0 + z);
            cols[static_cast<std::size_t>((x + 1) + (z + 1) * kB)] = c;
            max_height = std::max({max_height, c.height, c.water});
        }
    }
    const auto col = [&](int x, int z) -> const AdventureColumn& { return cols[static_cast<std::size_t>((x + 1) + (z + 1) * kB)]; };

    // --- Cuevas: densidad en una rejilla de 4 x 4 x 4 (interpolada) ---
    constexpr int kStep = 4;
    constexpr int kGx = kChunkSize / kStep + 1;
    const int gy_count = std::min(max_height + 8, kWorldHeight) / kStep + 2;
    std::vector<float> cave_grid(static_cast<std::size_t>(kGx * kGx * gy_count));
    for (int gy = 0; gy < gy_count; ++gy) {
        for (int gz = 0; gz < kGx; ++gz) {
            for (int gx = 0; gx < kGx; ++gx) {
                const float wx = static_cast<float>(x0 + gx * kStep);
                const float wy = static_cast<float>(gy * kStep);
                const float wz = static_cast<float>(z0 + gz * kStep);
                // Tuneles anchos: donde dos ruidos se anulan a la vez.
                const float a = noise3(wx / 56.0f, wy / 36.0f, wz / 56.0f, 211);
                const float b = noise3(wx / 56.0f, wy / 36.0f, wz / 56.0f, 223);
                const float tunnel = 0.11f - std::sqrt(a * a + b * b);
                // Cavernas grandes en lo hondo.
                const float cavern = (noise3(wx / 80.0f, wy / 44.0f, wz / 80.0f, 237) - 0.36f) *
                                     std::clamp((52.0f - wy) / 28.0f, 0.0f, 1.0f);
                cave_grid[static_cast<std::size_t>(gx + gz * kGx + gy * kGx * kGx)] = std::max(tunnel, cavern);
            }
        }
    }
    const auto cave = [&](int x, int y, int z) {
        const int gx = x / kStep, gy = y / kStep, gz = z / kStep;
        const float fx = static_cast<float>(x % kStep) / kStep, fy = static_cast<float>(y % kStep) / kStep,
                    fz = static_cast<float>(z % kStep) / kStep;
        const auto G = [&](int ix, int iy, int iz) { return cave_grid[static_cast<std::size_t>(ix + iz * kGx + iy * kGx * kGx)]; };
        const float c00 = lerp(G(gx, gy, gz), G(gx + 1, gy, gz), fx);
        const float c10 = lerp(G(gx, gy, gz + 1), G(gx + 1, gy, gz + 1), fx);
        const float c01 = lerp(G(gx, gy + 1, gz), G(gx + 1, gy + 1, gz), fx);
        const float c11 = lerp(G(gx, gy + 1, gz + 1), G(gx + 1, gy + 1, gz + 1), fx);
        return lerp(lerp(c00, c10, fz), lerp(c01, c11, fz), fy) > 0.0f;
    };

    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            const AdventureColumn& c = col(x, z);
            const int wx = x0 + x, wz = z0 + z;
            const bool underwater = c.water > c.height;  // mar o lago encima
            const int slope = std::max({std::abs(col(x + 1, z).height - c.height), std::abs(col(x - 1, z).height - c.height),
                                        std::abs(col(x, z + 1).height - c.height), std::abs(col(x, z - 1).height - c.height)});
            const bool cliff = slope >= 4 && !underwater;
            const int soil = 3 + static_cast<int>(rand01(wx, 0, wz, seed) * 2.0f);
            const int snowline = sea_ + 70 - static_cast<int>(c.temperature * 15.0f);
            // Entradas de cueva: solo en tierra alta, en algunas zonas.
            const bool cave_entrance = !underwater && c.height > sea_ + 6 && fbm2(static_cast<float>(wx) / 70.0f, static_cast<float>(wz) / 70.0f, 2, 120) > 0.3f;
            const float moss_patch = fbm2(static_cast<float>(wx) / 18.0f, static_cast<float>(wz) / 18.0f, 2, 121);

            // Tintes: hierba seca y amarilla con calor y sequia, verde intensa con humedad.
            const float dry = std::clamp(c.temperature * 0.5f - c.humidity * 0.6f + 0.3f, 0.0f, 1.0f);
            const float cold = std::clamp(-c.temperature - 0.2f, 0.0f, 1.0f);
            const float lush = std::clamp(c.humidity, 0.0f, 1.0f);
            const float tr = 0.80f + dry * 0.2f - cold * 0.1f - lush * 0.08f;
            const float tg = 1.0f - dry * 0.08f;
            const float tb = 0.72f - dry * 0.25f + cold * 0.18f;
            chunk.grass_tint[static_cast<std::size_t>(x + z * kChunkSize)] = packTint(tr, tg, tb);
            chunk.foliage_tint[static_cast<std::size_t>(x + z * kChunkSize)] = packTint(tr * 0.93f, tg, tb * 0.88f);

            // Agua de las columnas de al lado (las cuevas no se abren junto a ella).
            const AdventureColumn* side[4] = {&col(x + 1, z), &col(x - 1, z), &col(x, z + 1), &col(x, z - 1)};
            const auto water_beside = [&](int y) {
                for (const AdventureColumn* n : side) {
                    if (y > n->height && y < n->water) return true;
                }
                return false;
            };
            for (int y = 0; y <= std::min(std::max(c.height, c.water - 1), kWorldHeight - 1); ++y) {
                BlockId id = block::Air;
                if (y <= c.height) {
                    const int depth = c.height - y;
                    if (y == 0 || (y < 4 && rand01(wx, y, wz, seed + 3u) < 0.5f)) {
                        id = block::Bedrock;
                    } else if (depth == 0) {
                        switch (c.biome) {
                            case AB::AOcean: id = c.height < sea_ - 8 ? block::Gravel : (c.temperature > 0.2f ? block::Sand : block::Clay); break;
                            case AB::ARiver: {
                                const float r = rand01(wx, 1, wz, seed + 5u);
                                id = underwater ? (r < 0.5f ? block::Sand : (r < 0.8f ? block::Gravel : block::Clay))
                                                : (c.river > 0.5f ? block::Sand : block::Grass);
                                break;
                            }
                            case AB::ABeach: id = block::Sand; break;
                            case AB::ADesert: id = c.mesa ? block::RedSand : block::Sand; break;
                            case AB::ASnow: id = cliff ? block::Stone : (y > sea_ + 40 ? block::Snow : block::SnowyGrass); break;
                            case AB::AMountains:
                                id = y >= snowline ? block::Snow : (cliff || y > sea_ + 62 ? block::Stone : block::Grass);
                                break;
                            case AB::AForest:
                                id = cliff ? block::Stone : (moss_patch > 0.3f ? block::Moss : block::Grass);
                                break;
                            default: id = cliff ? block::Stone : block::Grass; break;
                        }
                    } else if (c.biome == AB::ADesert && c.mesa && y > sea_ + 2) {
                        // Capas de la meseta: franjas de terracota, arena roja y arenisca.
                        const int band = (y + static_cast<int>(fbm2(static_cast<float>(wx) / 40.0f, static_cast<float>(wz) / 40.0f, 1, 122) * 4.0f) + 64) % 7;
                        id = band < 3 ? block::Terracotta : (band == 3 ? block::RedSand : (band < 6 ? block::Terracotta : block::Sandstone));
                    } else if (depth < soil) {
                        switch (c.biome) {
                            case AB::ADesert:
                            case AB::ABeach: id = depth < 3 ? block::Sand : block::Sandstone; break;
                            case AB::AOcean: id = c.height < sea_ - 8 ? block::Gravel : block::Sand; break;
                            case AB::ARiver: id = underwater ? block::Sand : block::Dirt; break;
                            case AB::AMountains: id = (cliff || y > sea_ + 62) ? block::Stone : block::Dirt; break;
                            default: id = cliff ? block::Stone : block::Dirt; break;
                        }
                    } else if (c.biome == AB::ADesert && depth < soil + 4) {
                        id = block::Sandstone;
                    } else {
                        id = block::Stone;
                        // Minerales por profundidad (vetas).
                        const float r = rand01(wx, y, wz, seed + 101u);
                        const float vein = noise3(static_cast<float>(wx) / 5.0f, static_cast<float>(y) / 5.0f, static_cast<float>(wz) / 5.0f, 53);
                        if (vein > 0.36f) {
                            if (y < 16 && r < 0.18f) id = block::DiamondOre;
                            else if (y < 32 && r < 0.3f) id = block::GoldOre;
                            else if (y < 24 && r < 0.26f) id = block::CrystalOre;
                            else if (y < 64 && r < 0.55f) id = block::IronOre;
                            else if (y >= 40 && y < 110 && r > 0.72f) id = block::CopperOre;
                            else if (y < 128) id = block::CoalOre;
                        }
                    }
                    // Cuevas: no se abren bajo el agua ni (salvo en las entradas) en la capa de arriba,
                    // ni pegadas al agua de la columna de al lado: son secas (el agua son bloques
                    // y solo esta donde el mar o un lago la ponen).
                    if (id != block::Bedrock && y > 4 && cave(x, y, z) && !(underwater && depth < 6) &&
                        !(depth < 3 && !cave_entrance) && !(c.height < sea_ + 3 && depth < 4) && !water_beside(y)) {
                        id = block::Air;
                    }
                } else if (y < c.water) {
                    id = block::Water;
                }
                chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))] = id;
            }
            // Agua helada en el frio.
            if (underwater && c.temperature < -0.45f && c.water - 1 < kWorldHeight) {
                chunk.blocks[static_cast<std::size_t>(cellIndex(x, c.water - 1, z))] = block::Ice;
            }

            // --- Suelo de las cuevas: musgo, setas y setas luminosas ---
            for (int y = 6; y < c.height - 3; ++y) {
                const BlockId floor = chunk.get(x, y, z);
                if ((floor != block::Stone && floor != block::Dirt) || chunk.get(x, y + 1, z) != block::Air ||
                    chunk.get(x, y + 2, z) != block::Air) {
                    continue;
                }
                const float r = rand01(wx, y, wz, seed + 201u);
                if (r < 0.025f) {
                    chunk.blocks[static_cast<std::size_t>(cellIndex(x, y + 1, z))] = block::GlowMushroom;
                } else if (r < 0.045f) {
                    chunk.blocks[static_cast<std::size_t>(cellIndex(x, y + 1, z))] = block::BrownMushroom;
                }
                if (rand01(wx, y, wz, seed + 202u) < 0.3f) chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))] = block::Moss;
            }

            // --- Vegetacion sobre el suelo ---
            const int top = c.height;
            if (top + 2 >= kWorldHeight || underwater || chunk.get(x, top + 1, z) != block::Air) continue;
            const BlockId ground = chunk.get(x, top, z);
            const float r = rand01(wx, 7, wz, seed + 9u);
            const float field = fbm2(static_cast<float>(wx) / 45.0f, static_cast<float>(wz) / 45.0f, 2, 130);
            const float kind = fbm2(static_cast<float>(wx) / 25.0f, static_cast<float>(wz) / 25.0f, 1, 131) * 0.5f + 0.5f;
            const auto flower = [&]() -> BlockId {
                const float k = std::clamp(kind + (rand01(wx, 8, wz, seed) - 0.5f) * 0.3f, 0.0f, 0.999f);
                static constexpr BlockId kFlowers[] = {block::RedFlower, block::YellowFlower, block::BlueFlower,
                                                       block::WhiteFlower, block::Lavender};
                return kFlowers[static_cast<int>(k * 5.0f)];
            };
            BlockId plant = block::Air;
            const bool green = ground == block::Grass || ground == block::Moss;
            // Juncos en las orillas.
            if ((ground == block::Grass || ground == block::Sand) && c.river > 0.15f && top <= sea_ + 1) {
                if (r < 0.4f) plant = block::Reeds;
            } else {
                switch (c.biome) {
                    case AB::AMeadow:
                        if (!green) break;
                        if (field > 0.22f) plant = r < 0.32f ? flower() : (r < 0.8f ? block::TallGrass : block::Air);
                        else plant = r < 0.55f ? block::TallGrass : (r < 0.61f ? flower() : (r < 0.63f ? block::Bush : block::Air));
                        break;
                    case AB::AForest:
                        if (!green) break;
                        if (r < 0.22f) plant = block::Fern;
                        else if (r < 0.47f) plant = block::TallGrass;
                        else if (r < 0.53f) plant = block::Bush;
                        else if (r < 0.545f) plant = block::RedMushroom;
                        else if (r < 0.56f) plant = block::BrownMushroom;
                        else if (r < 0.58f) plant = flower();
                        break;
                    case AB::ADesert:
                        if (ground == block::Sand) plant = r < 0.035f ? block::DryGrass : (r < 0.05f ? block::DeadBush : block::Air);
                        else if (ground == block::RedSand && r < 0.02f) plant = block::DryGrass;
                        break;
                    case AB::ASnow:
                        if (ground == block::SnowyGrass) plant = r < 0.06f ? block::Fern : (r < 0.1f ? block::DryGrass : block::Air);
                        break;
                    case AB::AMountains:
                        if (ground == block::Grass) {
                            plant = r < 0.35f ? block::TallGrass : (r < 0.4f ? block::Lavender : (r < 0.43f ? block::WhiteFlower : block::Air));
                        }
                        break;
                    case AB::ABeach:
                    case AB::ARiver:
                        if (r < 0.03f) plant = block::DryGrass;
                        break;
                    default: break;
                }
            }
            if (plant == block::Air) continue;
            chunk.blocks[static_cast<std::size_t>(cellIndex(x, top + 1, z))] = plant;
            // Juncos de dos alturas a veces.
            if (plant == block::Reeds && r < 0.15f && chunk.get(x, top + 2, z) == block::Air) {
                chunk.blocks[static_cast<std::size_t>(cellIndex(x, top + 2, z))] = block::Reeds;
            }
        }
    }
    placeAdventureTrees(chunk);
    // Aldeas, ciudades, torres, ruinas y mazmorras (Structures.cpp).
    if (structures_) structures_->stamp(chunk);
}

// --- Arboles ------------------------------------------------------------------------------

// Candidatos en una rejilla de 4 x 4 del mundo con su posicion al azar
// (determinista); se prueban los de alrededor del chunk y se escribe la parte
// que cae dentro (copas de hasta 5 de radio).
void Generator::placeAdventureTrees(ChunkData& chunk) const {
    const int x0 = chunk.cx * kChunkSize;
    const int z0 = chunk.cz * kChunkSize;
    constexpr int kCell = 4;
    constexpr int kReach = 6;
    const auto put = [&](int wx, int y, int wz, BlockId id, bool only_air) {
        const int lx = wx - x0, lz = wz - z0;
        if (lx < 0 || lz < 0 || lx >= kChunkSize || lz >= kChunkSize || y < 1 || y >= kWorldHeight) return;
        BlockId& cell = chunk.blocks[static_cast<std::size_t>(cellIndex(lx, y, lz))];
        if (only_air && cell != block::Air && !blockDef(cell).replaceable) return;
        cell = id;
    };
    const std::uint32_t s = static_cast<std::uint32_t>(seed_) + 977u;
    const float clear_margin = treeClearMargin();  // franja libre junto a aldeas y caminos
    const int gx0 = static_cast<int>(std::floor(static_cast<float>(x0 - kReach) / kCell));
    const int gx1 = static_cast<int>(std::floor(static_cast<float>(x0 + kChunkSize + kReach) / kCell));
    const int gz0 = static_cast<int>(std::floor(static_cast<float>(z0 - kReach) / kCell));
    const int gz1 = static_cast<int>(std::floor(static_cast<float>(z0 + kChunkSize + kReach) / kCell));
    for (int gz = gz0; gz <= gz1; ++gz) {
        for (int gx = gx0; gx <= gx1; ++gx) {
            const int tx = gx * kCell + static_cast<int>(rand01(gx, 1, gz, s) * kCell);
            const int tz = gz * kCell + static_cast<int>(rand01(gx, 2, gz, s) * kCell);
            const AdventureColumn c = adventureColumn(tx, tz);
            if (c.height < sea_ + 1 || c.height > kWorldHeight - 30 || c.river > 0.2f || c.lake) continue;
            if (structures_ && structures_->blocksTrees(tx, tz, clear_margin)) continue;  // sin arboles en aldeas y ciudades
            float chance = 0.0f;
            switch (c.biome) {
                case AB::AForest: chance = 0.55f; break;
                case AB::AMeadow: chance = 0.035f; break;
                case AB::ASnow: chance = 0.22f; break;
                case AB::AMountains: chance = c.height < sea_ + 66 ? 0.14f : 0.0f; break;
                case AB::ADesert: chance = c.mesa ? 0.02f : 0.06f; break;  // cactus
                case AB::ABeach: chance = 0.07f; break;                    // palmeras
                default: break;
            }
            chance = treeChance(chance, tx, tz);  // densidad y claros (VoxelWorld)
            if (rand01(gx, 3, gz, s) >= chance) continue;
            // Nada en las laderas empinadas.
            const int slope = std::max(std::abs(adventureColumn(tx + 1, tz).height - c.height),
                                       std::abs(adventureColumn(tx, tz + 1).height - c.height));
            if (slope >= 3) continue;
            const int base = c.height + 1;
            const float kind = rand01(gx, 4, gz, s);
            const float size = rand01(gx, 5, gz, s);

            if (c.biome == AB::ADesert) {
                // Cactus, a veces con un brazo.
                const int h = 2 + static_cast<int>(size * 3.0f);
                for (int i = 0; i < h; ++i) put(tx, base + i, tz, block::Cactus, true);
                if (h >= 3 && kind < 0.4f) {
                    const int dir = static_cast<int>(kind * 10.0f) % 4;
                    const int dx = dir == 0 ? 1 : (dir == 1 ? -1 : 0), dz = dir == 2 ? 1 : (dir == 3 ? -1 : 0);
                    put(tx + dx, base + 1, tz + dz, block::Cactus, true);
                    put(tx + dx, base + 2, tz + dz, block::Cactus, true);
                }
                continue;
            }
            if (c.biome == AB::ABeach) {
                // Palmera: tronco y hojas que caen en cruz.
                const int h = 5 + static_cast<int>(size * 3.0f);
                for (int i = 0; i < h; ++i) put(tx, base + i, tz, block::OakLog, false);
                const int top = base + h;
                put(tx, top, tz, block::OakLeaves, true);
                static constexpr int kDirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
                for (int d = 0; d < 8; ++d) {
                    const int length = d < 4 ? 3 : 2;
                    for (int i = 1; i <= length; ++i) {
                        put(tx + kDirs[d][0] * i, top - (i >= 2 ? 1 : 0), tz + kDirs[d][1] * i, block::OakLeaves, true);
                    }
                }
                continue;
            }
            if (c.biome == AB::ASnow || c.biome == AB::AMountains) {
                // Abeto: tronco alto y capas de ramas que se estrechan.
                const int h = 8 + static_cast<int>(size * 5.0f);
                for (int i = 0; i < h; ++i) put(tx, base + i, tz, block::SpruceLog, false);
                for (int i = 2; i <= h + 1; ++i) {
                    const float k = static_cast<float>(i - 2) / static_cast<float>(h - 1);  // 0 abajo, 1 arriba
                    int radius = static_cast<int>(std::lround((1.0f - k) * 3.2f));
                    if ((h - i) % 3 == 0 && radius > 0) --radius;  // capas
                    for (int dz = -radius; dz <= radius; ++dz) {
                        for (int dx = -radius; dx <= radius; ++dx) {
                            if (std::abs(dx) + std::abs(dz) > radius + 1) continue;
                            if (dx == 0 && dz == 0 && i < h) continue;
                            put(tx + dx, base + i, tz + dz, block::SpruceLeaves, true);
                        }
                    }
                }
                put(tx, base + h + 1, tz, block::SpruceLeaves, true);
                continue;
            }
            // Roble de copa redonda (algunos enormes en el bosque) o abedul.
            const bool birch = c.biome == AB::AForest && kind < 0.3f;
            const bool giant = c.biome == AB::AForest && !birch && kind > 0.92f;
            const BlockId log = birch ? block::BirchLog : block::OakLog;
            const BlockId leaves = birch ? block::BirchLeaves : block::OakLeaves;
            const int h = birch ? 6 + static_cast<int>(size * 3.0f) : (giant ? 8 + static_cast<int>(size * 3.0f) : 4 + static_cast<int>(size * 3.0f));
            const float radius = birch ? 1.8f : (giant ? 4.2f : 2.3f + size * 1.2f);
            const float radius_y = birch ? 2.8f : radius * 0.8f;
            const int center = base + h - (birch ? 1 : 0);
            const int ri = static_cast<int>(std::ceil(radius));
            const int ry = static_cast<int>(std::ceil(radius_y));
            for (int dy = -ry; dy <= ry; ++dy) {
                for (int dz = -ri; dz <= ri; ++dz) {
                    for (int dx = -ri; dx <= ri; ++dx) {
                        const float d = (static_cast<float>(dx * dx + dz * dz)) / (radius * radius) +
                                        static_cast<float>(dy * dy) / (radius_y * radius_y);
                        if (d > 1.0f) continue;
                        // Borde irregular: la capa de fuera, a medias.
                        if (d > 0.7f && rand01(tx + dx, center + dy, tz + dz, s + 9u) < 0.35f) continue;
                        put(tx + dx, center + dy, tz + dz, leaves, true);
                    }
                }
            }
            for (int i = 0; i < h; ++i) put(tx, base + i, tz, log, false);
            // Raices de los gigantes: el tronco de 2 x 2.
            if (giant) {
                for (int i = 0; i < h - 2; ++i) {
                    put(tx + 1, base + i, tz, log, false);
                    put(tx, base + i, tz + 1, log, false);
                    put(tx + 1, base + i, tz + 1, log, false);
                }
            }
            put(tx, base - 1, tz, block::Dirt, false);
        }
    }
}

}  // namespace cramion::voxel
