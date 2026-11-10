// Terreno lejano (estilo "Distant Horizons"): lo que se ve de una columna
// desde lejos, directamente del generador y sin generar el chunk (ni cuevas,
// ni minerales, ni la luz). Lo usa FarTerrain.cpp para las mallas lejanas.
//
//   - Altura, bloque de arriba (con las mismas reglas por bioma que el chunk
//     de verdad), el de debajo y la roca de los acantilados.
//   - Agua: el mar y los lagos de montana (Aventura).
//   - Arboles: las copas, con la misma rejilla y las mismas probabilidades que
//     el generador (densidad y claros de VoxelWorld incluidos): desde lejos un
//     bosque es una masa de hojas a la altura de las copas.
//
// Es una funcion pura de (semilla, x, z), como el resto del generador: se
// llama desde cualquier hilo.

#include "VoxelInternal.h"
#include "Structures.h"

#include <algorithm>
#include <cmath>

namespace cramion::voxel {

namespace {

// Las mismas que Generator.cpp y GeneratorAdventure.cpp (alli son internas).
std::uint32_t hash3(int x, int y, int z, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^
                      static_cast<std::uint32_t>(z) * 83492791u ^ seed * 2654435761u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}
float rand01(int x, int y, int z, std::uint32_t seed) { return static_cast<float>(hash3(x, y, z, seed) & 0xFFFFFF) / 16777215.0f; }
std::uint16_t packTint(float r, float g, float b) {
    const auto c = [](float v) { return static_cast<std::uint16_t>(std::clamp(static_cast<int>(std::lround(v * 15.0f)), 0, 15)); };
    return static_cast<std::uint16_t>(c(r) | (c(g) << 4) | (c(b) << 8));
}
int floorDivInt(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }

// Biomas del Clasico (Generator.cpp).
enum ClassicBiome : int { COcean = 0, CBeach, CPlains, CForest, CDesert, CSnowy, CMountains };

}  // namespace

bool Generator::farColumn(int x, int z, FarColumn& out, bool detailed) const {
    out = FarColumn{};
    float chance = 0.0f;  // de arbol en la celda (la del bioma de esta columna)
    int cell = 4;
    int reach = 3;          // NATURALEZA-HZ: hasta donde llega la copa (6 con los arboles gigantes)
    bool cactus = false;    // NATURALEZA-HZ: saguaros (solo su columna)
    bool forest = false;
    std::uint32_t tree_seed = 0;
    bool trees = false;
    if (profile_ == VoxelGenerator::Adventure) {
        using AB = AdventureBiome;
        const AdventureColumn c = adventureColumn(x, z);
        int h = c.height;
        if (detailed && structures_) h = structures_->groundHeight(x, z, h);  // aldeas y ciudades aplanan
        out.height = h;
        out.water = c.water > h ? c.water : 0;
        out.sea = out.water != 0 && out.water == sea_;
        const int snowline = sea_ + 70 - static_cast<int>(c.temperature * 15.0f);
        const bool underwater = out.water != 0;
        switch (c.biome) {
            case AB::AOcean:
                if (c.ocean > 0.0f) {  // oceano hondo (GeneratorAdventure.cpp)
                    out.top = seabedBlock(c, x, z, 0);
                    out.under = out.deep = seabedBlock(c, x, z, 2);
                    break;
                }
                out.top = h < sea_ - 8 ? block::Gravel : (c.temperature > 0.2f ? block::Sand : block::Clay);
                out.under = h < sea_ - 8 ? block::Gravel : block::Sand;
                break;
            case AB::ARiver:
                out.top = underwater || c.river > 0.5f ? block::Sand : block::Grass;
                out.under = underwater ? block::Sand : block::Dirt;
                break;
            case AB::ABeach:
                out.top = block::Sand;
                out.under = block::Sand;
                out.deep = block::Sandstone;
                break;
            case AB::ADesert:
                out.top = c.mesa ? block::RedSand : block::Sand;
                out.under = c.mesa ? block::Terracotta : block::Sand;
                out.deep = c.mesa ? block::Terracotta : block::Sandstone;
                break;
            case AB::ASnow:
                out.top = h > sea_ + 40 ? block::Snow : block::SnowyGrass;
                out.cliffable = true;
                break;
            case AB::AMountains:
                out.top = h >= snowline ? block::Snow : (h > sea_ + 62 ? block::Stone : block::Grass);
                out.under = h > sea_ + 62 ? block::Stone : block::Dirt;
                out.cliffable = !((c.giant || c.peak) && h >= snowline);  // cumbres gigantes: nieve tambien de lejos
                break;
            // Tierras salvajes (GeneratorWild.cpp: wildBlock).
            case AB::ABadlands:
                out.top = block::RedSand;
                out.under = block::Terracotta;
                out.deep = block::Terracotta;  // paredes de capas
                out.cliffable = true;
                break;
            case AB::ARoseDesert:
                out.top = block::PinkSand;
                out.under = block::PinkSand;
                out.deep = block::PinkSandstone;
                out.cliffable = true;
                break;
            case AB::AAshlands:
                out.top = fbm2(static_cast<float>(x) / 20.0f, static_cast<float>(z) / 20.0f, 2, 260) > 0.05f ? block::Ash : block::Basalt;
                out.under = block::Basalt;
                out.deep = block::Basalt;
                out.cliffable = true;
                break;
            case AB::ABlight:
                out.top = underwater ? block::DuskSoil : block::BlightGrass;
                out.under = block::DuskSoil;
                out.deep = block::DuskStone;
                out.cliffable = true;
                break;
            default:
                out.top = block::Grass;
                out.cliffable = true;
                break;
        }
        // Relieve suelto de las tierras salvajes: chimeneas de hadas y puas.
        if ((c.feature == 1 || c.feature == 2) && h == c.height) {
            out.top = out.under = out.deep = c.feature == 2 ? block::Obsidian : block::RedSandstone;
        }
        // Lava (charcas y crateres): de lejos, la superficie de la lava.
        if (c.lava > c.height && c.lava > h && out.water == 0) {
            out.height = c.lava - 1;
            out.top = block::Lava;
            out.under = out.deep = block::Basalt;
            out.cliffable = false;
        }
        // Tintes (como generateAdventure).
        const float dry = std::clamp(c.temperature * 0.5f - c.humidity * 0.6f + 0.3f, 0.0f, 1.0f);
        const float cold = std::clamp(-c.temperature - 0.2f, 0.0f, 1.0f);
        const float lush = std::clamp(c.humidity, 0.0f, 1.0f);
        const float tr = 0.80f + dry * 0.2f - cold * 0.1f - lush * 0.08f;
        const float tg = 1.0f - dry * 0.08f;
        const float tb = 0.72f - dry * 0.25f + cold * 0.18f;
        out.grass_tint = packTint(tr, tg, tb);
        out.foliage_tint = packTint(tr * 0.93f, tg, tb * 0.88f);
        // Arboles (placeAdventureTrees): rejilla de 4 m.
        if (h >= sea_ + 1 && h <= kWorldHeight - 30 && c.river <= 0.2f && !c.lake && !underwater) {
            switch (c.biome) {
                case AB::AForest: chance = 0.55f; break;
                case AB::AMeadow: chance = 0.035f; break;
                case AB::ASnow: chance = 0.22f; break;
                case AB::AMountains: chance = h < sea_ + 66 ? 0.14f : 0.0f; break;
                default: break;  // desierto (cactus) y playa (palmeras): no se ven de lejos
            }
            // NATURALEZA-HZ: fuera de las regiones del origen, los saguaros se ven de lejos y los
            // arboles gigantes del bosque tienen la copa mas ancha y alta (placeAdventureTrees).
            if (natureNew(x, z)) {
                if (c.biome == AB::ADesert || c.biome == AB::ABadlands) {
                    chance = c.biome == AB::ABadlands ? 0.012f : (c.mesa ? 0.02f : 0.06f);
                    cactus = true;
                    reach = 0;
                } else if (c.biome == AB::AForest) {
                    forest = true;
                    reach = 6;
                }
            }
        }
        out.leaves = c.biome == AB::ASnow || c.biome == AB::AMountains ? block::SpruceLeaves : (cactus ? block::Cactus : block::OakLeaves);
        if (isLushBiome(c.biome)) lushFar(c, x, z, out);  // NOCHE-MUNDO: bloques, tintes y copas (GeneratorLush.cpp)
        cell = 4;
        tree_seed = static_cast<std::uint32_t>(seed_) + 977u;
        trees = chance > 0.0f;
        // Islas flotantes (GeneratorWild.cpp: placeWild): la misma silueta (sin las cuevas).
        {
            std::vector<WildIsland> islands;
            wildIslandsNear(x, z, x, z, detailed, islands);
            const int floor_y = std::max(out.height, sea_) + 10;
            for (const WildIsland& isl : islands) {
                WildIslandColumn c;
                if (!wildIslandColumn(isl, x, z, c)) continue;
                const int bottom = std::max(c.bottom, floor_y);
                if (bottom > c.top) continue;
                const int top = std::max(c.top, c.water);
                if (out.island_top == 0) {
                    out.island_bottom = bottom;
                    out.island_top = top;
                } else {
                    out.island_bottom = std::min(out.island_bottom, bottom);
                    out.island_top = std::max(out.island_top, top);
                }
                out.island_block = c.water > c.top ? block::Water : (c.rock ? (c.top >= 226 ? block::Snow : block::Stone) : block::Grass);
            }
            if (out.island_top != 0) {
                out.grass_tint = packTint(0.70f, 1.0f, 0.62f);  // como placeWild
                out.foliage_tint = packTint(0.66f, 1.0f, 0.56f);
            }
        }
    } else if (profile_ == VoxelGenerator::Classic) {
        const Column c = column(x, z);
        const int h = c.height;
        out.height = h;
        out.water = h < sea_ ? sea_ : 0;
        out.sea = out.water != 0;
        const bool underwater = out.water != 0;
        switch (c.biome) {
            case COcean:
                out.top = h < sea_ - 8 ? block::Gravel : (c.temperature > 0.2f ? block::Sand : block::Clay);
                out.under = h < sea_ - 8 ? block::Gravel : block::Sand;
                break;
            case CBeach:
            case CDesert:
                out.top = block::Sand;
                out.under = block::Sand;
                out.deep = block::Sandstone;
                break;
            case CSnowy: out.top = block::SnowyGrass; break;
            case CMountains:
                out.top = h > sea_ + 85 ? block::Snow : (h > sea_ + 65 ? block::Stone : block::Grass);
                out.under = h > sea_ + 65 ? block::Stone : block::Dirt;
                break;
            default: out.top = underwater ? block::Dirt : block::Grass; break;
        }
        const float dry = std::clamp(c.temperature * 0.5f - c.humidity * 0.6f + 0.3f, 0.0f, 1.0f);
        const float cold = std::clamp(-c.temperature - 0.2f, 0.0f, 1.0f);
        const float r = 0.82f + dry * 0.18f - cold * 0.1f;
        const float g = 1.0f - dry * 0.08f;
        const float b = 0.78f - dry * 0.25f + cold * 0.15f;
        out.grass_tint = packTint(r, g, b);
        out.foliage_tint = packTint(r * 0.95f, g, b * 0.9f);
        if (h >= sea_ + 1 && h <= kWorldHeight - 20) {
            switch (c.biome) {
                case CForest: chance = 0.62f; break;
                case CPlains: chance = 0.07f; break;
                case CSnowy: chance = 0.28f; break;
                case CMountains: chance = h < sea_ + 70 ? 0.12f : 0.0f; break;
                default: break;
            }
        }
        out.leaves = c.biome == CSnowy ? block::SpruceLeaves : block::OakLeaves;
        cell = 5;
        tree_seed = static_cast<std::uint32_t>(seed_) + 77u;
        trees = chance > 0.0f;
    } else {
        return false;  // dimensiones: sin terreno lejano
    }

    // Copa: algun arbol de las celdas de alrededor cuya copa (~5 x 5) cubre
    // esta columna. La probabilidad, la del bioma de aqui (los vecinos casi
    // siempre comparten bioma) con la densidad y los claros del mundo.
    if (trees) {
        chance = treeChance(chance, x, z);
        if (chance > 0.0f) {
            const int gx0 = floorDivInt(x - reach, cell), gx1 = floorDivInt(x + reach, cell);  // ARBOLES-HZ: copas mas anchas
            const int gz0 = floorDivInt(z - reach, cell), gz1 = floorDivInt(z + reach, cell);
            const float margin = treeClearMargin();
            for (int gz = gz0; gz <= gz1 && out.canopy == 0; ++gz) {
                for (int gx = gx0; gx <= gx1; ++gx) {
                    if (rand01(gx, 3, gz, tree_seed) >= chance) continue;
                    const int tx = gx * cell + static_cast<int>(rand01(gx, 1, gz, tree_seed) * static_cast<float>(cell));
                    const int tz = gz * cell + static_cast<int>(rand01(gx, 2, gz, tree_seed) * static_cast<float>(cell));
                    const float size = rand01(gx, 5, gz, tree_seed);
                    // NATURALEZA-HZ: el arbol gigante antiguo (kind > 0.9, size > 0.72) llega a 6; los demas a 3.
                    const bool ancient = forest && rand01(gx, 4, gz, tree_seed) > 0.9f && size > 0.72f && natureNew(tx, tz);
                    const int r = cactus ? 0 : (ancient ? 6 : 3);
                    if (std::abs(tx - x) > r || std::abs(tz - z) > r) continue;
                    if (detailed && structures_ && structures_->blocksTrees(tx, tz, margin)) continue;
                    if (cactus) {
                        if (rand01(gx, 4, gz, tree_seed) >= 0.45f) continue;  // solo los saguaros
                        out.canopy = std::min(out.height + 5 + static_cast<int>(size * 5.0f), kWorldHeight - 1);
                        break;
                    }
                    out.canopy = std::min(out.height + (ancient ? 24 : 7 + static_cast<int>(size * 5.0f)), kWorldHeight - 1);  // ARBOLES-HZ: mas altos
                    break;
                }
            }
        }
    }
    return true;
}

}  // namespace cramion::voxel
