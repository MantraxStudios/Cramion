// NOCHE-MUNDO: biomas frondosos de las tierras salvajes del generador Aventura.
//
// Ocupan las provincias (GeneratorWild.cpp: wildRegion) que antes eran "tierra
// normal" lejos del origen (mas de ~1,4 km; cerca el mundo es el de siempre):
//
//   - Verdant Jungle (WJungle / AJungle): colinas redondas, cesped de la selva,
//     arboles gigantes de 25-40 m (tronco de 2 x 2, raices, ramas con copas y
//     lianas colgando), arboles medianos, matas, helechos dobles.
//   - Blossom Vale (WBlossom / ABlossom): lomas suaves, cesped con petalos,
//     cerezos en flor (copas rosas), petalos caidos, flores y estanques con
//     nenufares.
//   - Mirefen Swamp (WSwamp / ASwamp): llano bajo lleno de charcas poco hondas,
//     barro, cesped del pantano, manglares sobre raices (tambien en el agua),
//     juncos, setas y nenufares.
//   - Sunscorch Savanna (WSavanna / ASavanna): llanura seca con mesetas bajas,
//     hierba alta seca (dos de alto), acacias de copa plana y termiteros.
//   - Emberleaf Woods (WAutumn / AAutumn): bosque de otono en colinas: robles y
//     abedules de hojas naranjas, rojas y amarillas sobre hojarasca.
//
// Todo es funcion pura de (semilla, x, z): el relieve va en adventureColumn
// (lushShape: lo ven terrainHeight, las estructuras y el terreno lejano) y los
// arboles se prueban en rejillas con un margen mayor que lo que ocupan (sin
// cortes entre chunks). Generator::setLushBiomes(false) deja el mundo de antes.

#include "VoxelInternal.h"
#include "Structures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

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
float rand01(int x, int y, int z, std::uint32_t seed) { return static_cast<float>(hash3(x, y, z, seed) & 0xFFFFFF) / 16777215.0f; }
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
int ifloor(float v) { return static_cast<int>(std::floor(v)); }

constexpr float kWildNear = 1400.0f;  // como GeneratorWild.cpp: hasta aqui, el mundo de siempre
// Rejillas de arboles: gigantes de la selva (celda grande) y el resto (celda pequena). El margen
// (reach) es mas de lo que ocupa un arbol desde su tronco: cada chunk prueba todos los que le tocan.
constexpr int kBigCell = 14, kBigReach = 13;
constexpr int kSmallCell = 5, kSmallReach = 9;

using AB = Generator::AdventureBiome;

// Escribe en el chunk lo que cae dentro (coordenadas del mundo).
struct Put {
    ChunkData& chunk;
    int x0 = 0, z0 = 0;
    bool inside(int wx, int y, int wz) const {
        const int lx = wx - x0, lz = wz - z0;
        return lx >= 0 && lz >= 0 && lx < kChunkSize && lz < kChunkSize && y >= 1 && y < kWorldHeight;
    }
    BlockId get(int wx, int y, int wz) const { return inside(wx, y, wz) ? chunk.get(wx - x0, y, wz - z0) : block::Air; }
    void put(int wx, int y, int wz, BlockId id, bool only_air) {
        if (!inside(wx, y, wz)) return;
        BlockId& cell = chunk.blocks[static_cast<std::size_t>(cellIndex(wx - x0, y, wz - z0))];
        if (only_air && cell != block::Air && !blockDef(cell).replaceable) return;
        cell = id;
    }
};

// Copa redonda (elipsoide) con el borde a medias. Centro en (cx + ox, cy, cz + oz) (ox, oz: 0 o 0.5).
void blob(Put& w, int cx, int cy, int cz, float ox, float oz, float r, float ry, BlockId leaves, std::uint32_t seed) {
    const int ri = static_cast<int>(std::ceil(r + ox + oz));
    const int ryi = static_cast<int>(std::ceil(ry));
    for (int dy = -ryi; dy <= ryi; ++dy) {
        for (int dz = -ri; dz <= ri; ++dz) {
            for (int dx = -ri; dx <= ri; ++dx) {
                const float fx = static_cast<float>(dx) + 0.5f - (0.5f + ox), fz = static_cast<float>(dz) + 0.5f - (0.5f + oz);
                const float d = (fx * fx + fz * fz) / (r * r) + static_cast<float>(dy * dy) / (ry * ry);
                if (d > 1.0f) continue;
                if (d > 0.72f && rand01(cx + dx, cy + dy, cz + dz, seed) < 0.38f) continue;
                w.put(cx + dx, cy + dy, cz + dz, leaves, true);
            }
        }
    }
}
// Disco plano (copas de acacia).
void disc(Put& w, int cx, int cy, int cz, float r, BlockId leaves, std::uint32_t seed) {
    const int ri = static_cast<int>(std::ceil(r));
    for (int dz = -ri; dz <= ri; ++dz) {
        for (int dx = -ri; dx <= ri; ++dx) {
            const float d = static_cast<float>(dx * dx + dz * dz) / (r * r);
            if (d > 1.0f) continue;
            if (d > 0.7f && rand01(cx + dx, cy, cz + dz, seed) < 0.35f) continue;
            w.put(cx + dx, cy, cz + dz, leaves, true);
        }
    }
}
// Tronco (o raiz, o rama) recto de a a b, bloque a bloque.
void beam(Put& w, float ax, float ay, float az, float bx, float by, float bz, BlockId log) {
    const float len = std::max({std::abs(bx - ax), std::abs(by - ay), std::abs(bz - az), 1.0f});
    const int steps = static_cast<int>(std::ceil(len * 2.0f));
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        w.put(ifloor(lerp(ax, bx, t)), ifloor(lerp(ay, by, t)), ifloor(lerp(az, bz, t)), log, false);
    }
}
// Lianas que cuelgan de la parte de abajo de una copa (anillo de radio r alrededor de (cx, cz)).
void vines(Put& w, float cx, int bottom, float cz, float r, int count, int max_len, std::uint32_t seed, int id) {
    for (int i = 0; i < count; ++i) {
        if (rand01(id, 40 + i, 0, seed) < 0.35f) continue;
        const float a = (static_cast<float>(i) + rand01(id, 60 + i, 0, seed) * 0.6f) / static_cast<float>(count) * 6.2831853f;
        const float rr = r * (0.65f + 0.3f * rand01(id, 80 + i, 0, seed));
        const int vx = ifloor(cx + std::cos(a) * rr), vz = ifloor(cz + std::sin(a) * rr);
        const int len = 2 + static_cast<int>(rand01(id, 100 + i, 0, seed) * static_cast<float>(max_len - 1));
        // Desde la hoja mas baja de la copa (en este chunk) hacia el suelo.
        int y = bottom - 2;
        if (!w.inside(vx, y, vz)) continue;
        while (y < bottom + 5 && w.get(vx, y, vz) == block::Air) ++y;
        if (y >= bottom + 5) continue;  // la copa no llega aqui
        for (int k = 1; k <= len; ++k) {
            if (w.get(vx, y - k, vz) != block::Air) break;
            w.put(vx, y - k, vz, block::JungleVines, true);
        }
    }
}

BlockId autumnLeaves(float k) {
    if (k < 0.40f) return block::AutumnLeavesOrange;
    if (k < 0.70f) return block::AutumnLeavesRed;
    if (k < 0.95f) return block::AutumnLeavesYellow;
    return block::OakLeaves;  // alguno verde todavia (con el tinte del otono, verde dorado)
}

// Probabilidad de arbol por celda de la rejilla pequena segun el bioma.
float smallChance(int biome) {
    switch (biome) {
        case AB::AJungle: return 0.42f;
        case AB::ABlossom: return 0.13f;
        case AB::ASwamp: return 0.17f;
        case AB::ASavanna: return 0.06f;
        case AB::AAutumn: return 0.46f;
        default: return 0.0f;
    }
}
constexpr float kGiantChance = 0.55f;  // gigantes de la selva por celda de la rejilla grande

// Rincones (templo, santuario, cabana, circulo de piedras, atalaya): uno como mucho por celda.
constexpr int kRuinCell = 224, kRuinClear = 11;
struct LushRuin {
    int x = 0, z = 0, base = 0, biome = 0;
    std::uint32_t seed = 0;
};

}  // namespace

// --- Relieve -------------------------------------------------------------------------------

void Generator::lushShape(float x, float z, float land, int stage, float k, float& h, AdventureColumn& c) const {
    const float sea = static_cast<float>(sea_);
    // El suelo "suave" de cada bioma (sin el detalle): las charcas se nivelan con el.
    const auto swampBase = [&]() { return sea + 4.0f + land * 1.5f + fbm2(x / 90.0f, z / 90.0f, 2, 621) * 1.6f; };
    const auto blossomBase = [&]() { return sea + 7.0f + land * 8.0f + fbm2(x / 180.0f, z / 180.0f, 3, 611) * 9.0f; };
    if (stage == 1) {
        switch (c.region) {
            case WJungle: {
                // Colinas redondas y apretadas.
                const float r = 1.0f - std::abs(fbm2(x / 120.0f, z / 120.0f, 3, 601));
                const float base = sea + 6.0f + land * 12.0f + r * r * 22.0f + fbm2(x / 45.0f, z / 45.0f, 2, 602) * 4.0f;
                h = lerp(h, base, k * 0.9f);
                break;
            }
            case WBlossom:
                h = lerp(h, blossomBase() + fbm2(x / 60.0f, z / 60.0f, 2, 612) * 2.5f, k * 0.9f);
                break;
            case WSwamp:
                h = lerp(h, swampBase() + fbm2(x / 25.0f, z / 25.0f, 1, 622) * 0.6f, k);
                break;
            case WSavanna: {
                float base = sea + 6.0f + land * 6.0f + fbm2(x / 240.0f, z / 240.0f, 3, 631) * 5.0f + fbm2(x / 50.0f, z / 50.0f, 2, 632) * 1.2f;
                // Mesetas bajas sueltas (escalones de 4 m).
                const float mn = fbm2(x / 300.0f, z / 300.0f, 3, 633) + fbm2(x / 70.0f, z / 70.0f, 2, 634) * 0.06f;
                if (mn > 0.30f) base = std::max(base, sea + 10.0f + std::floor(std::min((mn - 0.30f) * 140.0f, 16.0f) / 4.0f) * 4.0f);
                h = lerp(h, base, k);
                break;
            }
            case WAutumn: {
                const float r = 1.0f - std::abs(fbm2(x / 400.0f, z / 400.0f, 3, 642));
                const float base = sea + 8.0f + land * 10.0f + fbm2(x / 170.0f, z / 170.0f, 4, 641) * 15.0f + r * r * r * 18.0f;
                h = lerp(h, base, k * 0.9f);
                break;
            }
            default: break;
        }
        return;
    }
    // stage 2 (despues de los rios): charcas del pantano y estanques de los cerezos.
    if (c.region != WSwamp && c.region != WBlossom) return;
    if (c.river > 0.01f || k < 0.85f) return;
    const bool swamp = c.region == WSwamp;
    const float lp = swamp ? fbm2(x / 34.0f, z / 34.0f, 2, 625) + fbm2(x / 12.0f, z / 12.0f, 1, 626) * 0.12f
                           : fbm2(x / 55.0f, z / 55.0f, 2, 615);
    const float bank = swamp ? 0.02f : 0.34f, inner = swamp ? 0.09f : 0.40f;
    if (lp < bank) return;
    const int level = static_cast<int>(std::lround(swamp ? swampBase() : blossomBase())) + 1;  // primer bloque sin agua
    if (lp < inner) {
        h = std::max(h, static_cast<float>(level) - 1.0f);  // orilla a ras del agua: la contiene
    } else {
        const float deep = smoothstep(inner, inner + (swamp ? 0.2f : 0.15f), lp) * (swamp ? 1.99f : 2.99f);
        h = std::min(h, static_cast<float>(level) - 2.0f - std::floor(deep));
        c.pond = level;
    }
}

// --- Bloques y plantas ---------------------------------------------------------------------

BlockId Generator::lushBlock(const AdventureColumn& c, int x, int y, int z, int depth, int soil, bool cliff, bool underwater) const {
    const float fx = static_cast<float>(x), fz = static_cast<float>(z);
    switch (c.biome) {
        case AB::AJungle:
            if (depth == 0) {
                if (cliff) return fbm2(fx / 9.0f, fz / 9.0f, 1, 603) > -0.1f ? block::Moss : block::Stone;
                return underwater ? block::Dirt : block::JungleGrass;
            }
            if (depth < soil) return cliff ? block::Stone : block::Dirt;
            return block::Air;
        case AB::ABlossom:
            if (depth == 0) {
                if (cliff) return block::Stone;
                if (underwater) return rand01(x, 1, z, static_cast<std::uint32_t>(seed_) + 613u) < 0.5f ? block::Clay : block::Sand;
                return block::BlossomGrass;
            }
            if (depth < soil) return cliff ? block::Stone : (underwater ? block::Clay : block::Dirt);
            return block::Air;
        case AB::ASwamp: {
            const bool mud = underwater || fbm2(fx / 14.0f, fz / 14.0f, 2, 627) > 0.33f;
            if (depth == 0) return mud ? block::Mud : block::SwampGrass;
            if (depth < 2) return block::Mud;
            if (depth < soil + 1) return y < sea_ ? block::Clay : block::Dirt;
            return block::Air;
        }
        case AB::ASavanna:
            if (depth == 0) return cliff ? block::Terracotta : block::SavannaGrass;
            if (depth < soil) return cliff ? block::Terracotta : block::Dirt;
            if (cliff && depth < soil + 5) return (y / 3) % 2 == 0 ? block::Terracotta : block::RedSandstone;
            return block::Air;
        case AB::AAutumn:
            if (depth == 0) {
                if (cliff) return block::Stone;
                if (underwater) return block::Dirt;
                return fbm2(fx / 16.0f, fz / 16.0f, 2, 647) > 0.28f ? block::Grass : block::LeafLitter;
            }
            if (depth < soil) return cliff ? block::Stone : block::Dirt;
            return block::Air;
        default: return block::Air;
    }
}

BlockId Generator::lushPlant(const AdventureColumn& c, BlockId ground, int x, int z, float r) const {
    (void)x;
    (void)z;
    switch (c.biome) {
        case AB::AJungle:
            if (ground != block::JungleGrass && ground != block::Moss) return block::Air;
            if (r < 0.28f) return block::Fern;
            if (r < 0.52f) return block::TallGrass;
            if (r < 0.62f) return block::Bush;
            if (r < 0.63f) return block::RedFlower;
            if (r < 0.64f) return block::BrownMushroom;
            return block::Air;
        case AB::ABlossom:
            if (ground != block::BlossomGrass) return block::Air;
            if (r < 0.22f) return block::BlossomPetals;
            if (r < 0.48f) return block::TallGrass;
            if (r < 0.54f) return block::WhiteFlower;
            if (r < 0.58f) return block::Lavender;
            if (r < 0.60f) return block::BlueFlower;
            return block::Air;
        case AB::ASwamp:
            if (ground != block::SwampGrass && ground != block::Mud) return block::Air;
            if (r < 0.20f) return block::TallGrass;
            if (r < 0.30f) return block::Reeds;
            if (r < 0.36f) return block::Fern;
            if (r < 0.39f) return block::BrownMushroom;
            if (r < 0.40f) return block::RedMushroom;
            return block::Air;
        case AB::ASavanna:
            if (ground != block::SavannaGrass) return block::Air;
            if (r < 0.42f) return block::DryGrass;
            if (r < 0.50f) return block::TallGrass;
            if (r < 0.52f) return block::DeadBush;
            if (r < 0.525f) return block::YellowFlower;
            return block::Air;
        case AB::AAutumn:
            if (ground != block::LeafLitter && ground != block::Grass) return block::Air;
            if (r < 0.12f) return block::Fern;
            if (r < 0.28f) return ground == block::Grass ? block::TallGrass : block::Air;
            if (r < 0.33f) return block::Bush;
            if (r < 0.36f) return block::RedMushroom;
            if (r < 0.40f) return block::BrownMushroom;
            if (r < 0.41f) return block::YellowFlower;
            return block::Air;
        default: return block::Air;
    }
}

void Generator::lushTint(const AdventureColumn& c, std::uint16_t& grass, std::uint16_t& foliage) const {
    switch (c.biome) {
        case AB::AJungle: grass = packTint(0.60f, 1.0f, 0.48f); foliage = packTint(0.52f, 1.0f, 0.42f); break;
        case AB::ABlossom: grass = packTint(0.84f, 1.0f, 0.70f); foliage = packTint(0.80f, 1.0f, 0.66f); break;
        case AB::ASwamp: grass = packTint(0.70f, 0.82f, 0.46f); foliage = packTint(0.62f, 0.78f, 0.40f); break;
        case AB::ASavanna: grass = packTint(1.0f, 0.92f, 0.42f); foliage = packTint(0.90f, 0.92f, 0.44f); break;
        case AB::AAutumn: grass = packTint(1.0f, 0.86f, 0.45f); foliage = packTint(1.0f, 0.62f, 0.30f); break;
        default: break;
    }
}

// --- Arboles -------------------------------------------------------------------------------

void Generator::placeLushTrees(ChunkData& chunk) const {
    if (!lush_biomes_ || profile_ != VoxelGenerator::Adventure) return;
    const int x0 = chunk.cx * kChunkSize, z0 = chunk.cz * kChunkSize;
    {
        // Cerca del origen no hay biomas frondosos (el mundo de siempre).
        const float nx = std::clamp(0.0f, static_cast<float>(x0 - 40), static_cast<float>(x0 + kChunkSize + 40));
        const float nz = std::clamp(0.0f, static_cast<float>(z0 - 40), static_cast<float>(z0 + kChunkSize + 40));
        if (nx * nx + nz * nz < (kWildNear - 100.0f) * (kWildNear - 100.0f)) return;
    }
    Put w{chunk, x0, z0};
    const auto s = static_cast<std::uint32_t>(seed_) + 7717u;
    const float clear_margin = treeClearMargin();
    const auto entrances = entrancesTouching(x0 - kBigReach, z0 - kBigReach, x0 + kChunkSize + kBigReach, z0 + kChunkSize + kBigReach);
    // Si el arbol puede ir en (tx, tz): devuelve la columna (o biome -1).
    const auto site = [&](int tx, int tz, bool allow_water) {
        AdventureColumn c = adventureColumn(tx, tz);
        const bool wet = c.water > c.height;
        if (!isLushBiome(c.biome) || c.height > kWorldHeight - 48 || c.river > 0.2f || c.lake || c.feature != 0 ||
            (wet && !(allow_water && c.pond > c.height && c.water - c.height <= 3)) || (!wet && c.height < sea_ + 1) ||
            (structures_ && structures_->blocksTrees(tx, tz, clear_margin)) ||
            (!entrances.empty() && nearCaveEntrance(entrances, tx, tz, 2.0f)) || caveOpenNear(tx, tz)) {
            c.biome = -1;
        }
        return c;
    };
    const auto steep = [&](int tx, int tz, int h) {
        return std::max(std::abs(adventureColumn(tx + 1, tz).height - h), std::abs(adventureColumn(tx, tz + 1).height - h)) >= 3;
    };
    // Rincones que tocan este chunk o los arboles que se prueban (sus sitios no dependen del chunk).
    std::vector<LushRuin> ruins;
    {
        const int reach = kBigReach + kRuinClear + 2;
        for (int gz = floorDiv(z0 - reach, kRuinCell); gz <= floorDiv(z0 + kChunkSize + reach, kRuinCell); ++gz) {
            for (int gx = floorDiv(x0 - reach, kRuinCell); gx <= floorDiv(x0 + kChunkSize + reach, kRuinCell); ++gx) {
                if (rand01(gx, 70, gz, s) > 0.5f) continue;
                const int rx = gx * kRuinCell + 24 + static_cast<int>(rand01(gx, 71, gz, s) * (kRuinCell - 48));
                const int rz = gz * kRuinCell + 24 + static_cast<int>(rand01(gx, 72, gz, s) * (kRuinCell - 48));
                if (rx + reach < x0 || rx - reach >= x0 + kChunkSize || rz + reach < z0 || rz - reach >= z0 + kChunkSize) continue;
                const AdventureColumn c = adventureColumn(rx, rz);
                const bool wet = c.water > c.height;
                if (!isLushBiome(c.biome) || c.strength < 0.6f || c.river > 0.05f || c.lake || c.feature != 0) continue;
                if ((wet && (c.biome != AB::ASwamp || c.water - c.height > 3)) || (!wet && c.height < sea_ + 1)) continue;
                if (c.height > kWorldHeight - 40) continue;
                if (structures_ && structures_->blocksTrees(rx, rz, 16.0f)) continue;
                if (caveOpenNear(rx, rz)) continue;
                const auto near_entrances = entrancesTouching(rx - 10, rz - 10, rx + 10, rz + 10);
                if (!near_entrances.empty() && nearCaveEntrance(near_entrances, rx, rz, 9.0f)) continue;
                int lo = c.height, hi = c.height;
                for (const int oz : {-6, 6}) {
                    for (const int ox : {-6, 6}) {
                        const int h = adventureColumn(rx + ox, rz + oz).height;
                        lo = std::min(lo, h);
                        hi = std::max(hi, h);
                    }
                }
                if (hi - lo > (c.biome == AB::ASavanna ? 6 : 4)) continue;
                ruins.push_back(LushRuin{rx, rz, std::max(c.height + 1, c.water), c.biome, hash3(gx, 73, gz, s)});
            }
        }
    }
    const auto nearRuin = [&](int tx, int tz) {
        for (const LushRuin& r : ruins) {
            if (std::abs(tx - r.x) <= kRuinClear && std::abs(tz - r.z) <= kRuinClear) return true;
        }
        return false;
    };

    // --- Gigantes de la selva (rejilla grande) ---
    for (int gz = floorDiv(z0 - kBigReach, kBigCell); gz <= floorDiv(z0 + kChunkSize + kBigReach, kBigCell); ++gz) {
        for (int gx = floorDiv(x0 - kBigReach, kBigCell); gx <= floorDiv(x0 + kChunkSize + kBigReach, kBigCell); ++gx) {
            const int tx = gx * kBigCell + 2 + static_cast<int>(rand01(gx, 1, gz, s) * (kBigCell - 4));
            const int tz = gz * kBigCell + 2 + static_cast<int>(rand01(gx, 2, gz, s) * (kBigCell - 4));
            if (rand01(gx, 3, gz, s) >= treeChance(kGiantChance, tx, tz) || nearRuin(tx, tz)) continue;
            const AdventureColumn c = site(tx, tz, false);
            if (c.biome != AB::AJungle || steep(tx, tz, c.height)) continue;
            const int id = static_cast<int>(hash3(gx, 4, gz, s) & 0x7FFFFFFF);
            const float size = rand01(gx, 5, gz, s);
            const int base = c.height + 1;
            const int H = std::min(24 + static_cast<int>(size * 14.0f), kWorldHeight - 8 - base);
            const float cx = static_cast<float>(tx) + 1.0f, cz = static_cast<float>(tz) + 1.0f;  // centro del 2 x 2
            // Copa principal y un remate.
            const float R = 6.0f + size * 1.8f;
            blob(w, tx, base + H, tz, 0.5f, 0.5f, R, 2.7f, block::JungleLeaves, s + 11u);
            blob(w, tx, base + H + 2, tz, 0.5f, 0.5f, R * 0.55f, 2.0f, block::JungleLeaves, s + 12u);
            // Ramas con su copa.
            const int branches = 2 + static_cast<int>(rand01(gx, 6, gz, s) * 2.0f);
            for (int b = 0; b < branches; ++b) {
                const float a = (static_cast<float>(b) + rand01(gx, 10 + b, gz, s) * 0.7f) / static_cast<float>(branches) * 6.2831853f;
                const float len = 3.0f + rand01(gx, 20 + b, gz, s) * 2.5f;
                const float by = static_cast<float>(base) + static_cast<float>(H) * (0.55f + 0.22f * rand01(gx, 30 + b, gz, s));
                const float ex = cx + std::cos(a) * len, ez = cz + std::sin(a) * len, ey = by + len * 0.6f;
                beam(w, cx, by, cz, ex, ey, ez, block::JungleLog);
                const float rb = 2.6f + rand01(gx, 40 + b, gz, s) * 1.2f;
                blob(w, ifloor(ex), ifloor(ey) + 1, ifloor(ez), 0.0f, 0.0f, rb, 1.8f, block::JungleLeaves, s + 13u);
                vines(w, ex, ifloor(ey), ez, rb, 5, 5, s + 14u, id + 7 * b);
            }
            // Tronco de 2 x 2 y raices (contrafuertes).
            for (int y = base - 1; y < base + H; ++y) {
                for (int dz = 0; dz < 2; ++dz) {
                    for (int dx = 0; dx < 2; ++dx) w.put(tx + dx, y, tz + dz, block::JungleLog, false);
                }
            }
            static constexpr int kSides[8][2] = {{-1, 0}, {-1, 1}, {2, 0}, {2, 1}, {0, -1}, {1, -1}, {0, 2}, {1, 2}};
            for (int i = 0; i < 8; ++i) {
                if (rand01(gx, 50 + i, gz, s) < 0.45f) continue;
                const int rh = 1 + static_cast<int>(rand01(gx, 60 + i, gz, s) * 4.0f);
                for (int y = base - 1; y < base + rh; ++y) w.put(tx + kSides[i][0], y, tz + kSides[i][1], block::JungleLog, false);
                const int ox = kSides[i][0] < 0 ? -1 : (kSides[i][0] > 1 ? 1 : 0), oz = kSides[i][1] < 0 ? -1 : (kSides[i][1] > 1 ? 1 : 0);
                if (rh >= 3) w.put(tx + kSides[i][0] + ox, base, tz + kSides[i][1] + oz, block::JungleLog, true);
            }
            // Lianas de la copa grande.
            vines(w, cx, base + H - 2, cz, R, 16, 9, s + 15u, id);
        }
    }

    // --- El resto (rejilla pequena) ---
    for (int gz = floorDiv(z0 - kSmallReach, kSmallCell); gz <= floorDiv(z0 + kChunkSize + kSmallReach, kSmallCell); ++gz) {
        for (int gx = floorDiv(x0 - kSmallReach, kSmallCell); gx <= floorDiv(x0 + kChunkSize + kSmallReach, kSmallCell); ++gx) {
            const int tx = gx * kSmallCell + static_cast<int>(rand01(gx, 1, gz, s + 1u) * kSmallCell);
            const int tz = gz * kSmallCell + static_cast<int>(rand01(gx, 2, gz, s + 1u) * kSmallCell);
            const float roll = rand01(gx, 3, gz, s + 1u);
            if (roll >= 0.5f || nearRuin(tx, tz)) continue;  // (ninguna probabilidad pasa de 0.5: ahorra la columna)
            const AdventureColumn c = site(tx, tz, true);
            if (c.biome < 0 || roll >= treeChance(smallChance(c.biome), tx, tz)) continue;
            const bool wet = c.water > c.height;
            if (!wet && steep(tx, tz, c.height)) continue;
            const int id = static_cast<int>(hash3(gx, 4, gz, s + 1u) & 0x7FFFFFFF);
            const float kind = rand01(gx, 4, gz, s + 1u), size = rand01(gx, 5, gz, s + 1u);
            const int base = c.height + 1;
            const float fx = static_cast<float>(tx) + 0.5f, fz = static_cast<float>(tz) + 0.5f;
            switch (c.biome) {
                case AB::AJungle: {
                    if (kind < 0.4f) {  // mata: un tronco y hojas
                        w.put(tx, base, tz, block::JungleLog, false);
                        blob(w, tx, base + 1, tz, 0.0f, 0.0f, 1.6f + size * 0.6f, 1.3f, block::JungleLeaves, s + 21u);
                        break;
                    }
                    const int h = 7 + static_cast<int>(size * 7.0f);
                    const float r = 2.6f + size * 1.2f;
                    blob(w, tx, base + h, tz, 0.0f, 0.0f, r, 2.0f, block::JungleLeaves, s + 22u);
                    for (int y = base; y < base + h; ++y) w.put(tx, y, tz, block::JungleLog, false);
                    vines(w, fx, base + h - 1, fz, r, 7, 5, s + 23u, id);
                    break;
                }
                case AB::ABlossom: {
                    // Cerezo: tronco que se tuerce a media altura y copas rosas anchas.
                    const int h = 4 + static_cast<int>(size * 3.0f);
                    const int dir = static_cast<int>(kind * 4.0f) & 3;
                    const int dx = dir == 0 ? 1 : (dir == 1 ? -1 : 0), dz = dir == 2 ? 1 : (dir == 3 ? -1 : 0);
                    const int bend = h / 2;
                    for (int y = base; y < base + h; ++y) {
                        const bool upper = y >= base + bend;
                        w.put(tx + (upper ? dx : 0), y, tz + (upper ? dz : 0), block::CherryLog, false);
                    }
                    const int top = base + h;
                    const float r = 3.0f + size * 1.4f;
                    blob(w, tx + dx, top, tz + dz, 0.0f, 0.0f, r, 2.1f, block::CherryLeaves, s + 31u);
                    // Rama hacia el otro lado con su copa.
                    beam(w, fx, static_cast<float>(base + bend), fz, fx - static_cast<float>(dx) * 2.5f, static_cast<float>(base + bend) + 1.5f,
                         fz - static_cast<float>(dz) * 2.5f, natureNew(tx, tz) ? block::Branch : block::CherryLog);  // RAMAS-HZ
                    blob(w, tx - dx * 2, base + bend + 2, tz - dz * 2, 0.0f, 0.0f, r * 0.65f, 1.6f, block::CherryLeaves, s + 32u);
                    break;
                }
                case AB::ASwamp: {
                    // Mangle: tronco alzado sobre raices en arco (tambien dentro del agua).
                    const int floor_y = wet ? c.water : base;  // donde empieza: la superficie del agua o el suelo
                    const int y0 = floor_y + 2;
                    const int h = 5 + static_cast<int>(size * 3.0f);
                    const int roots = 4 + static_cast<int>(kind * 3.0f);
                    for (int i = 0; i < roots; ++i) {
                        const float a = (static_cast<float>(i) + rand01(gx, 10 + i, gz, s + 1u) * 0.5f) / static_cast<float>(roots) * 6.2831853f;
                        const float d = 2.0f + rand01(gx, 20 + i, gz, s + 1u) * 1.5f;
                        const float ex = fx + std::cos(a) * d, ez = fz + std::sin(a) * d;
                        const int ground = adventureColumn(ifloor(ex), ifloor(ez)).height;
                        beam(w, fx, static_cast<float>(y0), fz, ex, static_cast<float>(y0) - 1.0f, ez, block::MangroveLog);
                        for (int y = y0 - 1; y > ground; --y) w.put(ifloor(ex), y, ifloor(ez), block::MangroveLog, false);
                    }
                    for (int y = y0; y < y0 + h; ++y) w.put(tx, y, tz, block::MangroveLog, false);
                    const float r = 3.0f + size * 1.4f;
                    blob(w, tx, y0 + h, tz, 0.0f, 0.0f, r, 1.8f, block::MangroveLeaves, s + 41u);
                    vines(w, fx, y0 + h - 1, fz, r, 6, 4, s + 42u, id);
                    break;
                }
                case AB::ASavanna: {
                    if (kind < 0.22f) {
                        // Termitero: cono de terracota.
                        const int mh = 2 + static_cast<int>(size * 3.0f);
                        for (int y = 0; y < mh; ++y) {
                            const float rr = 1.4f * (1.0f - static_cast<float>(y) / static_cast<float>(mh)) + 0.35f;
                            const int ri = static_cast<int>(std::ceil(rr));
                            for (int dz = -ri; dz <= ri; ++dz) {
                                for (int dx = -ri; dx <= ri; ++dx) {
                                    if (static_cast<float>(dx * dx + dz * dz) > rr * rr) continue;
                                    w.put(tx + dx, base - 1 + y, tz + dz, y == 0 ? block::Terracotta : (y % 2 ? block::Terracotta : block::RedSand), false);
                                }
                            }
                        }
                        w.put(tx, base - 1 + mh, tz, block::Terracotta, false);
                        break;
                    }
                    // Acacia: tronco, dos ramas en diagonal y copas planas.
                    const int h = 3 + static_cast<int>(size * 3.0f);
                    for (int y = base; y < base + h; ++y) w.put(tx, y, tz, block::AcaciaLog, false);
                    const float a = kind * 6.2831853f;
                    for (int b = 0; b < 2; ++b) {
                        const float ang = a + (b == 0 ? 0.0f : 3.1415926f + (rand01(gx, 7, gz, s + 1u) - 0.5f));
                        const float len = b == 0 ? 3.0f + size * 1.5f : 2.0f + size;
                        const float ex = fx + std::cos(ang) * len, ez = fz + std::sin(ang) * len;
                        const float ey = static_cast<float>(base + h) + len * 0.8f;
                        beam(w, fx, static_cast<float>(base + h - 1), fz, ex, ey, ez, natureNew(tx, tz) ? block::Branch : block::AcaciaLog);  // RAMAS-HZ
                        const float r = (b == 0 ? 3.2f : 2.4f) + size * 0.8f;
                        disc(w, ifloor(ex), ifloor(ey) + 1, ifloor(ez), r, block::AcaciaLeaves, s + 51u + static_cast<std::uint32_t>(b));
                        disc(w, ifloor(ex), ifloor(ey) + 2, ifloor(ez), r - 1.3f, block::AcaciaLeaves, s + 53u + static_cast<std::uint32_t>(b));
                    }
                    break;
                }
                case AB::AAutumn: {
                    const BlockId leaves = autumnLeaves(rand01(gx, 8, gz, s + 1u));
                    const bool birch = kind < 0.28f;
                    const bool big = !birch && kind > 0.93f;
                    const BlockId log = birch ? block::BirchLog : block::OakLog;
                    const int h = birch ? 6 + static_cast<int>(size * 3.0f) : (big ? 8 + static_cast<int>(size * 3.0f) : 4 + static_cast<int>(size * 3.0f));
                    const float r = birch ? 1.9f : (big ? 4.3f : 2.4f + size * 1.3f);
                    const float ry = birch ? 2.8f : r * 0.8f;
                    blob(w, tx, base + h - (birch ? 1 : 0), tz, 0.0f, 0.0f, r, ry, leaves, s + 61u);
                    for (int y = base; y < base + h; ++y) w.put(tx, y, tz, log, false);
                    break;
                }
                default: break;
            }
        }
    }

    // --- Rincones de cada bioma (uno como mucho por celda de 224 m; GeneratorLush ruins) ---
    // Templo cubierto de musgo en la selva, santuario con puerta roja entre los cerezos, cabana sobre
    // pilotes en el pantano, circulo de piedras con hoguera en la sabana y atalaya en ruinas en el otono.
    // Los arboles no crecen a menos de kRuinClear m de uno (lo decide la posicion del arbol: sin cortes).
    for (const LushRuin& r : ruins) {
        if (r.x + 9 < x0 || r.x - 9 >= x0 + kChunkSize || r.z + 9 < z0 || r.z - 9 >= z0 + kChunkSize) continue;
        const auto groundAt = [&](int x, int z) { return adventureColumn(x, z).height; };
        // Cimientos: rellena desde el suelo de cada columna hasta debajo de `y` (solo lo de este chunk).
        const auto footing = [&](int x, int z, int y, BlockId id) {
            if (!w.inside(x, y, z)) return;
            for (int k = groundAt(x, z) + 1; k < y; ++k) w.put(x, k, z, id, false);
        };
        const auto clear = [&](int x, int z, int y0, int y1) {
            for (int y = y0; y <= y1; ++y) w.put(x, y, z, block::Air, false);
        };
        const int b = r.base;
        switch (r.biome) {
            case AB::AJungle: {
                // Piramide escalonada de ladrillo musgoso con un santuario arriba y lianas.
                for (int dz = -6; dz <= 6; ++dz)
                    for (int dx = -6; dx <= 6; ++dx) clear(r.x + dx, r.z + dz, b, b + 12);
                for (int t = 0; t < 4; ++t) {
                    const int hs = 5 - t;
                    for (int dz = -hs; dz <= hs; ++dz) {
                        for (int dx = -hs; dx <= hs; ++dx) {
                            for (int y = b + 2 * t; y < b + 2 * t + 2; ++y) {
                                const float q = rand01(r.x + dx, y, r.z + dz, r.seed);
                                w.put(r.x + dx, y, r.z + dz, q < 0.55f ? block::MossyBricks : (q < 0.65f ? block::CrackedBricks : block::StoneBricks), false);
                            }
                            if (t == 0) footing(r.x + dx, r.z + dz, b, block::StoneBricks);
                        }
                    }
                    // Lianas por los lados del escalon.
                    for (int i = -hs; i <= hs; ++i) {
                        for (int side = 0; side < 4; ++side) {
                            const int vx = side < 2 ? r.x + (side == 0 ? hs + 1 : -hs - 1) : r.x + i;
                            const int vz = side < 2 ? r.z + i : r.z + (side == 2 ? hs + 1 : -hs - 1);
                            if (rand01(vx, t, vz, r.seed + 1u) < 0.3f) w.put(vx, b + 2 * t + 1, vz, block::JungleVines, true);
                        }
                    }
                }
                // Escalera de la cara +z y santuario de piedra tallada.
                for (int t = 0; t < 4; ++t) {
                    w.put(r.x, b + 2 * t, r.z + 6 - t, block::CarvedStone, false);
                    w.put(r.x, b + 2 * t + 1, r.z + 5 - t, block::CarvedStone, false);
                }
                const int top = b + 8;
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        for (int y = top; y < top + 3; ++y) {
                            const bool wall = std::abs(dx) == 1 || std::abs(dz) == 1;
                            const bool door = dx == 0 && dz == 1 && y < top + 2;
                            w.put(r.x + dx, y, r.z + dz, wall && !door ? block::CarvedStone : block::Air, false);
                        }
                        w.put(r.x + dx, top + 3, r.z + dz, block::MossyBricks, false);
                    }
                }
                w.put(r.x, top, r.z, block::Candles, false);
                break;
            }
            case AB::ABlossom: {
                // Santuario: losas, puerta roja (torii), altar con velas y farolillos.
                for (int dz = -4; dz <= 4; ++dz) {
                    for (int dx = -4; dx <= 4; ++dx) {
                        const bool slab = std::abs(dx) <= 3 && std::abs(dz) <= 3;
                        if (slab) {
                            footing(r.x + dx, r.z + dz, b - 1, block::Stone);
                            w.put(r.x + dx, b - 1, r.z + dz, block::Paving, false);
                        }
                        clear(r.x + dx, r.z + dz, b, b + 6);
                    }
                }
                for (const int side : {-2, 2}) {
                    for (int y = b; y < b + 4; ++y) w.put(r.x + side, y, r.z + 3, block::ClothRed, false);
                }
                for (int dx = -3; dx <= 3; ++dx) w.put(r.x + dx, b + 4, r.z + 3, block::DarkPlanks, false);
                for (int dx = -2; dx <= 2; ++dx) w.put(r.x + dx, b + 3, r.z + 3, block::ClothRed, false);
                w.put(r.x, b, r.z - 1, block::Altar, false);
                w.put(r.x, b + 1, r.z - 1, block::Candles, false);
                w.put(r.x - 2, b, r.z - 2, block::PaperLantern, false);
                w.put(r.x + 2, b, r.z - 2, block::PaperLantern, false);
                for (int i = 0; i < 6; ++i) {
                    const int px = r.x - 3 + static_cast<int>(rand01(i, 1, 0, r.seed) * 7.0f), pz = r.z - 3 + static_cast<int>(rand01(i, 2, 0, r.seed) * 7.0f);
                    w.put(px, b, pz, block::BlossomPetals, true);
                }
                break;
            }
            case AB::ASwamp: {
                // Cabana sobre pilotes de mangle con tejado de paja.
                const int fy = b + 2;
                for (int dz = -3; dz <= 3; ++dz)
                    for (int dx = -3; dx <= 3; ++dx) clear(r.x + dx, r.z + dz, fy, fy + 8);
                for (int dz = -2; dz <= 2; ++dz) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        w.put(r.x + dx, fy, r.z + dz, block::MangrovePlanks, false);
                        const bool edge = std::abs(dx) == 2 || std::abs(dz) == 2;
                        for (int y = fy + 1; y <= fy + 3; ++y) {
                            const bool door = dx == 0 && dz == 2 && y <= fy + 2;
                            const bool window = dz == 0 && std::abs(dx) == 2 && y == fy + 2;
                            w.put(r.x + dx, y, r.z + dz, edge && !door && !window ? block::MangrovePlanks : block::Air, false);
                        }
                    }
                }
                for (const int dz : {-2, 2})
                    for (const int dx : {-2, 2}) footing(r.x + dx, r.z + dz, fy, block::MangroveLog);
                w.put(r.x, fy, r.z + 3, block::MangrovePlanks, false);  // porche
                footing(r.x, r.z + 3, fy, block::MangroveLog);
                for (int k = 0; k < 4; ++k) {
                    const int hs = 3 - k;
                    for (int dz = -hs; dz <= hs; ++dz)
                        for (int dx = -hs; dx <= hs; ++dx) w.put(r.x + dx, fy + 4 + k, r.z + dz, block::Thatch, false);
                }
                w.put(r.x, fy + 3, r.z, block::Lantern, false);
                w.put(r.x + 1, fy + 1, r.z - 1, block::Barrel, false);
                w.put(r.x - 1, fy + 1, r.z - 1, block::Crate, false);
                break;
            }
            case AB::ASavanna: {
                // Circulo de piedras con una hoguera.
                for (int i = 0; i < 9; ++i) {
                    const float a = static_cast<float>(i) / 9.0f * 6.2831853f;
                    const int px = r.x + static_cast<int>(std::lround(std::cos(a) * 5.0f)), pz = r.z + static_cast<int>(std::lround(std::sin(a) * 5.0f));
                    if (!w.inside(px, 2, pz)) continue;
                    const int g = groundAt(px, pz);
                    const int hgt = 2 + static_cast<int>(rand01(i, 3, 0, r.seed) * 2.0f);
                    for (int y = g + 1; y <= g + hgt; ++y) w.put(px, y, pz, rand01(i, y, 1, r.seed) < 0.5f ? block::Cobblestone : block::Stone, false);
                    if (rand01(i, 4, 0, r.seed) < 0.4f) w.put(px, g + hgt + 1, pz, block::Stone, false);
                }
                if (w.inside(r.x, 2, r.z)) {
                    const int g = groundAt(r.x, r.z);
                    w.put(r.x, g + 1, r.z, block::Campfire, false);
                }
                break;
            }
            case AB::AAutumn: {
                // Atalaya de piedra en ruinas (la parte de arriba rota).
                for (int dz = -3; dz <= 3; ++dz)
                    for (int dx = -3; dx <= 3; ++dx) clear(r.x + dx, r.z + dz, b, b + 12);
                for (int dz = -2; dz <= 2; ++dz) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        footing(r.x + dx, r.z + dz, b - 1, block::Cobblestone);
                        w.put(r.x + dx, b - 1, r.z + dz, block::Paving, false);
                        if (std::abs(dx) != 2 && std::abs(dz) != 2) continue;
                        const int hgt = 7 + static_cast<int>(rand01(r.x + dx, 5, r.z + dz, r.seed) * 4.0f);
                        for (int y = b; y < b + hgt; ++y) {
                            if (dx == 0 && dz == -2 && y < b + 2) continue;  // puerta
                            if (dz == 0 && std::abs(dx) == 2 && y == b + 4) continue;  // aspilleras
                            w.put(r.x + dx, y, r.z + dz, rand01(r.x + dx, y, r.z + dz, r.seed) < 0.4f ? block::MossyBricks : block::Cobblestone, false);
                        }
                    }
                }
                w.put(r.x + 1, b, r.z + 1, block::Cobweb, false);
                w.put(r.x - 1, b + 5, r.z, block::Cobweb, true);
                w.put(r.x, b, r.z + 1, block::Crate, false);
                break;
            }
            default: break;
        }
    }
}

// --- Terreno lejano ------------------------------------------------------------------------

void Generator::lushFar(const AdventureColumn& c, int x, int z, FarColumn& out) const {
    const bool underwater = out.water != 0;
    const float fx = static_cast<float>(x), fz = static_cast<float>(z);
    out.under = block::Dirt;
    out.deep = block::Stone;
    out.cliffable = true;
    switch (c.biome) {
        case AB::AJungle: out.top = block::JungleGrass; out.leaves = block::JungleLeaves; break;
        case AB::ABlossom:
            out.top = underwater ? block::Clay : block::BlossomGrass;
            out.leaves = block::CherryLeaves;
            break;
        case AB::ASwamp:
            out.top = underwater || fbm2(fx / 14.0f, fz / 14.0f, 2, 627) > 0.33f ? block::Mud : block::SwampGrass;
            out.under = block::Mud;
            out.cliffable = false;
            out.leaves = block::MangroveLeaves;
            break;
        case AB::ASavanna:
            out.top = block::SavannaGrass;
            out.deep = block::Terracotta;
            out.leaves = block::AcaciaLeaves;
            break;
        case AB::AAutumn:
            out.top = fbm2(fx / 16.0f, fz / 16.0f, 2, 647) > 0.28f ? block::Grass : block::LeafLitter;
            out.leaves = block::AutumnLeavesOrange;
            break;
        default: return;
    }
    lushTint(c, out.grass_tint, out.foliage_tint);
    // Copas: los mismos candidatos que placeLushTrees (sin mirar el sitio de cada uno: la
    // probabilidad y la altura de esta columna), para que un bosque se vea como una masa de hojas.
    if (underwater || out.height < sea_ + 1 || c.river > 0.2f || c.lake) return;
    const auto s = static_cast<std::uint32_t>(seed_) + 7717u;
    int canopy = 0;
    if (c.biome == AB::AJungle) {
        const float chance = treeChance(kGiantChance, x, z);
        for (int gz = floorDiv(z - 7, kBigCell); gz <= floorDiv(z + 7, kBigCell); ++gz) {
            for (int gx = floorDiv(x - 7, kBigCell); gx <= floorDiv(x + 7, kBigCell); ++gx) {
                if (rand01(gx, 3, gz, s) >= chance) continue;
                const int tx = gx * kBigCell + 2 + static_cast<int>(rand01(gx, 1, gz, s) * (kBigCell - 4));
                const int tz = gz * kBigCell + 2 + static_cast<int>(rand01(gx, 2, gz, s) * (kBigCell - 4));
                const float size = rand01(gx, 5, gz, s);
                const float R = 6.0f + size * 1.8f;
                if (std::hypot(fx - static_cast<float>(tx) - 1.0f, fz - static_cast<float>(tz) - 1.0f) > R) continue;
                canopy = std::max(canopy, out.height + 1 + 24 + static_cast<int>(size * 14.0f) + 2);
            }
        }
    }
    const float chance = treeChance(smallChance(c.biome), x, z);
    for (int gz = floorDiv(z - 3, kSmallCell); gz <= floorDiv(z + 3, kSmallCell); ++gz) {
        for (int gx = floorDiv(x - 3, kSmallCell); gx <= floorDiv(x + 3, kSmallCell); ++gx) {
            const float roll = rand01(gx, 3, gz, s + 1u);
            if (roll >= 0.5f || roll >= chance) continue;
            const int tx = gx * kSmallCell + static_cast<int>(rand01(gx, 1, gz, s + 1u) * kSmallCell);
            const int tz = gz * kSmallCell + static_cast<int>(rand01(gx, 2, gz, s + 1u) * kSmallCell);
            if (std::abs(tx - x) > 3 || std::abs(tz - z) > 3) continue;
            const float kind = rand01(gx, 4, gz, s + 1u), size = rand01(gx, 5, gz, s + 1u);
            int h = 0;
            switch (c.biome) {
                case AB::AJungle: h = kind < 0.4f ? 2 : 9 + static_cast<int>(size * 7.0f); break;
                case AB::ABlossom: h = 6 + static_cast<int>(size * 3.0f); break;
                case AB::ASwamp: h = 9 + static_cast<int>(size * 3.0f); break;
                case AB::ASavanna: h = kind < 0.22f ? 0 : 7 + static_cast<int>(size * 4.0f); break;
                case AB::AAutumn: {
                    h = 5 + static_cast<int>(size * 3.0f);
                    out.leaves = autumnLeaves(rand01(gx, 8, gz, s + 1u));
                    break;
                }
                default: break;
            }
            if (h > 0) canopy = std::max(canopy, out.height + h);
        }
    }
    if (canopy > 0) out.canopy = std::min(canopy, kWorldHeight - 1);
}

}  // namespace cramion::voxel
