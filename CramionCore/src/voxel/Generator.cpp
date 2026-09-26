// Generador del mundo de bloques: una funcion pura de (semilla, x, z), asi
// que cada chunk se puede generar en cualquier hilo y en cualquier orden.
//
//   - Continentes y oceanos (ruido de muy baja frecuencia), colinas y
//     montanas (ruido "ridged" que se suma lejos de la costa).
//   - Biomas por temperatura y humedad (y altura): llanura, bosque,
//     desierto, nieve, montana; playas junto al mar.
//   - Capas: cesped/arena/nieve, tierra o arenisca y piedra; roca madre abajo.
//   - Cuevas: tuneles donde dos ruidos 3D casi se anulan a la vez, y cavernas
//     hondas; el ruido se muestrea en una rejilla de 4 x 4 x 4 y se
//     interpola (como Minecraft), 16 veces menos trabajo.
//   - Minerales por profundidad, arboles (roble, abedul, abeto, cactus) y
//     plantas. Los arboles de los chunks vecinos que se meten en este tambien
//     se dibujan aqui: el mundo no tiene cortes entre chunks.

#include "VoxelInternal.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace cramion::voxel {

namespace {

enum Biome : int { Ocean = 0, Beach, Plains, Forest, Desert, Snowy, Mountains };

std::uint32_t hash3(int x, int y, int z, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^
                      static_cast<std::uint32_t>(z) * 83492791u ^ seed * 2654435761u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}
float rand01(int x, int y, int z, std::uint32_t seed) { return static_cast<float>(hash3(x, y, z, seed) & 0xFFFFFF) / 16777215.0f; }

float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }

float grad2(int h, float x, float y) {
    switch (h & 7) {
        case 0: return x + y;
        case 1: return -x + y;
        case 2: return x - y;
        case 3: return -x - y;
        case 4: return x;
        case 5: return -x;
        case 6: return y;
        default: return -y;
    }
}
float grad3(int h, float x, float y, float z) {
    const int k = h & 15;
    const float u = k < 8 ? x : y;
    const float v = k < 4 ? y : (k == 12 || k == 14 ? x : z);
    return ((k & 1) ? -u : u) + ((k & 2) ? -v : v);
}

std::uint16_t packTint(float r, float g, float b) {
    const auto c = [](float v) { return static_cast<std::uint16_t>(std::clamp(static_cast<int>(std::lround(v * 15.0f)), 0, 15)); };
    return static_cast<std::uint16_t>(c(r) | (c(g) << 4) | (c(b) << 8));
}

}  // namespace

Generator::Generator(int seed, int sea_level) : seed_(seed), sea_(sea_level) {
    std::array<int, 256> p{};
    std::iota(p.begin(), p.end(), 0);
    std::mt19937 rng(static_cast<std::uint32_t>(seed));
    std::shuffle(p.begin(), p.end(), rng);
    for (int i = 0; i < 512; ++i) perm_[static_cast<std::size_t>(i)] = p[static_cast<std::size_t>(i & 255)];
}

float Generator::noise2(float x, float z, int salt) const {
    x += static_cast<float>(salt) * 131.7f;
    z += static_cast<float>(salt) * 71.3f;
    const int xi = static_cast<int>(std::floor(x)) & 255;
    const int zi = static_cast<int>(std::floor(z)) & 255;
    const float xf = x - std::floor(x);
    const float zf = z - std::floor(z);
    const float u = fade(xf), v = fade(zf);
    const auto& p = perm_;
    const int aa = p[static_cast<std::size_t>(p[static_cast<std::size_t>(xi)] + zi)];
    const int ab = p[static_cast<std::size_t>(p[static_cast<std::size_t>(xi)] + zi + 1)];
    const int ba = p[static_cast<std::size_t>(p[static_cast<std::size_t>(xi + 1)] + zi)];
    const int bb = p[static_cast<std::size_t>(p[static_cast<std::size_t>(xi + 1)] + zi + 1)];
    return lerp(lerp(grad2(aa, xf, zf), grad2(ba, xf - 1.0f, zf), u), lerp(grad2(ab, xf, zf - 1.0f), grad2(bb, xf - 1.0f, zf - 1.0f), u), v) *
           0.7f;
}

float Generator::fbm2(float x, float z, int octaves, int salt) const {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += noise2(x * freq, z * freq, salt + i * 17) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.03f;
    }
    return sum / norm;
}

float Generator::noise3(float x, float y, float z, int salt) const {
    x += static_cast<float>(salt) * 57.1f;
    const int xi = static_cast<int>(std::floor(x)) & 255;
    const int yi = static_cast<int>(std::floor(y)) & 255;
    const int zi = static_cast<int>(std::floor(z)) & 255;
    const float xf = x - std::floor(x), yf = y - std::floor(y), zf = z - std::floor(z);
    const float u = fade(xf), v = fade(yf), w = fade(zf);
    const auto P = [&](int i) { return perm_[static_cast<std::size_t>(i)]; };
    const int a = P(xi) + yi, aa = P(a) + zi, ab = P(a + 1) + zi;
    const int b = P(xi + 1) + yi, ba = P(b) + zi, bb = P(b + 1) + zi;
    return lerp(lerp(lerp(grad3(P(aa), xf, yf, zf), grad3(P(ba), xf - 1, yf, zf), u),
                     lerp(grad3(P(ab), xf, yf - 1, zf), grad3(P(bb), xf - 1, yf - 1, zf), u), v),
                lerp(lerp(grad3(P(aa + 1), xf, yf, zf - 1), grad3(P(ba + 1), xf - 1, yf, zf - 1), u),
                     lerp(grad3(P(ab + 1), xf, yf - 1, zf - 1), grad3(P(bb + 1), xf - 1, yf - 1, zf - 1), u), v),
                w);
}

Generator::Column Generator::column(int x, int z) const {
    const float fx = static_cast<float>(x), fz = static_cast<float>(z);
    Column c;
    // Continente: < 0 mar, > 0 tierra (con costas irregulares).
    const float continent = fbm2(fx / 900.0f, fz / 900.0f, 4, 1) + fbm2(fx / 180.0f, fz / 180.0f, 2, 2) * 0.12f + 0.12f;
    const float hills = fbm2(fx / 120.0f, fz / 120.0f, 4, 3);
    const float ridge = 1.0f - std::abs(fbm2(fx / 380.0f, fz / 380.0f, 4, 4));
    const float mountain_mask = std::clamp((fbm2(fx / 700.0f, fz / 700.0f, 3, 5) + 0.05f) * 2.2f, 0.0f, 1.0f);
    const float land = std::clamp(continent * 3.0f, -1.0f, 1.0f);
    float h;
    if (land < 0.0f) {
        h = static_cast<float>(sea_) + land * 28.0f + hills * 4.0f;  // fondo marino
    } else {
        const float inland = std::clamp(land * 2.0f, 0.0f, 1.0f);
        h = static_cast<float>(sea_) + 1.0f + land * 10.0f + hills * 10.0f * inland +
            std::pow(ridge, 3.0f) * 85.0f * mountain_mask * inland;
    }
    c.height = std::clamp(static_cast<int>(std::lround(h)), 4, kWorldHeight - 40);
    c.temperature = fbm2(fx / 1100.0f, fz / 1100.0f, 3, 6) * 1.6f - std::max(0.0f, (h - static_cast<float>(sea_) - 40.0f) / 60.0f);
    c.humidity = fbm2(fx / 900.0f, fz / 900.0f, 3, 7) * 1.6f;
    if (c.height < sea_ - 1) {
        c.biome = Ocean;
    } else if (c.height <= sea_ + 2 && land < 0.08f) {
        c.biome = c.temperature < -0.45f ? Snowy : Beach;
    } else if (c.height > sea_ + 55) {
        c.biome = Mountains;
    } else if (c.temperature < -0.35f) {
        c.biome = Snowy;
    } else if (c.temperature > 0.35f && c.humidity < 0.0f) {
        c.biome = Desert;
    } else if (c.humidity > 0.12f) {
        c.biome = Forest;
    } else {
        c.biome = Plains;
    }
    return c;
}

int Generator::terrainHeight(int x, int z) const { return column(x, z).height; }

void Generator::generate(ChunkData& chunk) const {
    const int x0 = chunk.cx * kChunkSize;
    const int z0 = chunk.cz * kChunkSize;
    std::array<Column, kChunkSize * kChunkSize> columns{};
    int max_height = 0;
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            const Column c = column(x0 + x, z0 + z);
            columns[static_cast<std::size_t>(x + z * kChunkSize)] = c;
            max_height = std::max(max_height, c.height);
            // Tintes: hierba seca y amarilla con calor y sequia, verde intensa con humedad.
            const float dry = std::clamp(c.temperature * 0.5f - c.humidity * 0.6f + 0.3f, 0.0f, 1.0f);
            const float cold = std::clamp(-c.temperature - 0.2f, 0.0f, 1.0f);
            const float r = 0.82f + dry * 0.18f - cold * 0.1f;
            const float g = 1.0f - dry * 0.08f;
            const float b = 0.78f - dry * 0.25f + cold * 0.15f;
            chunk.grass_tint[static_cast<std::size_t>(x + z * kChunkSize)] = packTint(r, g, b);
            chunk.foliage_tint[static_cast<std::size_t>(x + z * kChunkSize)] = packTint(r * 0.95f, g, b * 0.9f);
        }
    }

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
                // Tuneles: donde dos ruidos se anulan a la vez.
                const float a = noise3(wx / 48.0f, wy / 32.0f, wz / 48.0f, 11);
                const float b = noise3(wx / 48.0f, wy / 32.0f, wz / 48.0f, 23);
                const float tunnel = 0.085f - std::sqrt(a * a + b * b);
                // Cavernas grandes, solo en lo hondo.
                const float cavern = (noise3(wx / 70.0f, wy / 40.0f, wz / 70.0f, 37) - 0.42f) * std::clamp((40.0f - wy) / 25.0f, 0.0f, 1.0f);
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
            const Column& c = columns[static_cast<std::size_t>(x + z * kChunkSize)];
            const int wx = x0 + x, wz = z0 + z;
            const bool underwater = c.height < sea_;
            const int soil = 3 + static_cast<int>(rand01(wx, 0, wz, static_cast<std::uint32_t>(seed_)) * 2.0f);
            for (int y = 0; y <= std::min(std::max(c.height, sea_), kWorldHeight - 1); ++y) {
                BlockId id = block::Air;
                if (y <= c.height) {
                    const int depth = c.height - y;
                    if (y == 0 || (y < 4 && rand01(wx, y, wz, static_cast<std::uint32_t>(seed_) + 3u) < 0.5f)) {
                        id = block::Bedrock;
                    } else if (depth == 0) {
                        switch (c.biome) {
                            case Ocean: id = c.height < sea_ - 8 ? block::Gravel : (c.temperature > 0.2f ? block::Sand : block::Clay); break;
                            case Beach: id = block::Sand; break;
                            case Desert: id = block::Sand; break;
                            case Snowy: id = block::SnowyGrass; break;
                            case Mountains: id = y > sea_ + 85 ? block::Snow : (y > sea_ + 65 ? block::Stone : block::Grass); break;
                            default: id = underwater ? block::Dirt : block::Grass; break;
                        }
                    } else if (depth < soil) {
                        if (c.biome == Desert || c.biome == Beach) {
                            id = depth < 3 ? block::Sand : block::Sandstone;
                        } else if (c.biome == Ocean) {
                            id = c.height < sea_ - 8 ? block::Gravel : block::Sand;
                        } else if (c.biome == Mountains && y > sea_ + 65) {
                            id = block::Stone;
                        } else {
                            id = block::Dirt;
                        }
                    } else if (c.biome == Desert && depth < soil + 4) {
                        id = block::Sandstone;
                    } else {
                        id = block::Stone;
                        // Minerales por profundidad (vetas).
                        const float r = rand01(wx, y, wz, static_cast<std::uint32_t>(seed_) + 101u);
                        const float vein = noise3(static_cast<float>(wx) / 5.0f, static_cast<float>(y) / 5.0f, static_cast<float>(wz) / 5.0f, 53);
                        if (vein > 0.36f) {
                            if (y < 16 && r < 0.18f) id = block::DiamondOre;
                            else if (y < 32 && r < 0.3f) id = block::GoldOre;
                            else if (y < 64 && r < 0.55f) id = block::IronOre;
                            else if (y < 128) id = block::CoalOre;
                        }
                    }
                    // Cuevas (sin abrirse bajo el mar ni en la capa de arriba).
                    if (id != block::Bedrock && y > 4 && cave(x, y, z) && !(underwater && depth < 6) &&
                        !(depth < 2 && c.height < sea_ + 3)) {
                        id = block::Air;
                    }
                } else if (y < sea_) {
                    id = block::Water;
                }
                chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))] = id;
            }
            // Hielo en el mar frio.
            if (underwater && c.temperature < -0.5f) {
                chunk.blocks[static_cast<std::size_t>(cellIndex(x, sea_ - 1, z))] = block::Ice;
            }
            // Plantas sobre el suelo.
            const int top = c.height;
            if (top + 1 < kWorldHeight && !underwater && chunk.get(x, top + 1, z) == block::Air) {
                const BlockId ground = chunk.get(x, top, z);
                const float r = rand01(wx, 7, wz, static_cast<std::uint32_t>(seed_) + 9u);
                BlockId plant = block::Air;
                if (ground == block::Grass) {
                    const float density = c.biome == Forest ? 0.25f : 0.38f;
                    if (r < density) plant = block::TallGrass;
                    else if (r < density + 0.025f) plant = block::RedFlower;
                    else if (r < density + 0.05f) plant = block::YellowFlower;
                } else if (ground == block::Sand && c.biome == Desert && r < 0.012f) {
                    plant = block::DeadBush;
                }
                if (plant != block::Air) chunk.blocks[static_cast<std::size_t>(cellIndex(x, top + 1, z))] = plant;
            }
        }
    }
    placeTrees(chunk);
}

// Arboles: candidatos en una rejilla de 5 x 5 del mundo con su posicion al
// azar (determinista); se prueban los de alrededor del chunk y se escribe
// la parte que cae dentro.
void Generator::placeTrees(ChunkData& chunk) const {
    const int x0 = chunk.cx * kChunkSize;
    const int z0 = chunk.cz * kChunkSize;
    constexpr int kCell = 5;
    constexpr int kReach = 3;
    const auto put = [&](int wx, int y, int wz, BlockId id, bool only_air) {
        const int lx = wx - x0, lz = wz - z0;
        if (lx < 0 || lz < 0 || lx >= kChunkSize || lz >= kChunkSize || y < 1 || y >= kWorldHeight) return;
        BlockId& cell = chunk.blocks[static_cast<std::size_t>(cellIndex(lx, y, lz))];
        if (only_air && cell != block::Air && !blockDef(cell).replaceable) return;
        cell = id;
    };
    const int gx0 = static_cast<int>(std::floor(static_cast<float>(x0 - kReach) / kCell));
    const int gx1 = static_cast<int>(std::floor(static_cast<float>(x0 + kChunkSize + kReach) / kCell));
    const int gz0 = static_cast<int>(std::floor(static_cast<float>(z0 - kReach) / kCell));
    const int gz1 = static_cast<int>(std::floor(static_cast<float>(z0 + kChunkSize + kReach) / kCell));
    for (int gz = gz0; gz <= gz1; ++gz) {
        for (int gx = gx0; gx <= gx1; ++gx) {
            const std::uint32_t s = static_cast<std::uint32_t>(seed_) + 77u;
            const int tx = gx * kCell + static_cast<int>(rand01(gx, 1, gz, s) * kCell);
            const int tz = gz * kCell + static_cast<int>(rand01(gx, 2, gz, s) * kCell);
            const Column c = column(tx, tz);
            if (c.height < sea_ + 1 || c.height > kWorldHeight - 20) continue;
            float chance = 0.0f;
            switch (c.biome) {
                case Forest: chance = 0.62f; break;
                case Plains: chance = 0.07f; break;
                case Snowy: chance = 0.28f; break;
                case Mountains: chance = c.height < sea_ + 70 ? 0.12f : 0.0f; break;
                case Desert: chance = 0.12f; break;  // cactus
                default: break;
            }
            if (rand01(gx, 3, gz, s) >= chance) continue;
            const int base = c.height + 1;
            const float kind = rand01(gx, 4, gz, s);
            const int height_bonus = static_cast<int>(rand01(gx, 5, gz, s) * 3.0f);
            if (c.biome == Desert) {
                const int h = 1 + height_bonus;
                for (int i = 0; i < h; ++i) put(tx, base + i, tz, block::Cactus, true);
                continue;
            }
            if (c.biome == Snowy || (c.biome == Mountains && kind < 0.7f)) {
                // Abeto: tronco alto y capas de ramas que se estrechan.
                const int h = 7 + height_bonus;
                for (int i = 0; i < h; ++i) put(tx, base + i, tz, block::SpruceLog, false);
                for (int i = 2; i <= h + 1; ++i) {
                    const int radius = std::max(0, ((h + 1 - i) % 3 == 0 ? 1 : 2) - (i > h - 1 ? 1 : 0) + (i < h - 3 ? 1 : 0)) ;
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
            const bool birch = c.biome == Forest && kind < 0.35f;
            const BlockId log = birch ? block::BirchLog : block::OakLog;
            const BlockId leaves = birch ? block::BirchLeaves : block::OakLeaves;
            const int h = (birch ? 5 : 4) + height_bonus;
            // Copa: dos capas anchas y dos estrechas, con esquinas al azar.
            for (int dy = h - 2; dy <= h + 1; ++dy) {
                const int radius = dy >= h ? 1 : 2;
                for (int dz = -radius; dz <= radius; ++dz) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const bool corner = std::abs(dx) == radius && std::abs(dz) == radius;
                        if (corner && (dy == h + 1 || rand01(tx + dx, base + dy, tz + dz, s + 9u) < 0.55f)) continue;
                        put(tx + dx, base + dy, tz + dz, leaves, true);
                    }
                }
            }
            for (int i = 0; i < h; ++i) put(tx, base + i, tz, log, false);
            // El suelo bajo el tronco: tierra.
            put(tx, base - 1, tz, block::Dirt, false);
        }
    }
}

}  // namespace cramion::voxel
