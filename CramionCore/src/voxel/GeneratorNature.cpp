// NATURALEZA-HZ: relieve masivo y decoracion del suelo del generador Aventura.
//
//   - Cordilleras: zonas largas (~2 km) con crestas afiladas que llegan a y ~240 (unos 180
//     bloques sobre el mar), crestas secundarias, valles hondos entre ellas (el suelo de
//     siempre) y nieve y hielo arriba (el clima ya enfria con la altura). Las cumbres por
//     encima de ~sea + 120 son "gigantes" (paredes de roca desnuda, como GeneratorWild).
//   - Mesetas: islas de tierra alta de cima llana y escalones de 9 m con acantilados.
//   - Afloramientos (como Hytale): rocas de 2 a ~20 bloques que asoman del suelo con la veta
//     a la vista; el mineral depende del bioma y de la altura (carbon y hierro en todas
//     partes, cobre abajo, oro en montana y desierto, geodas de cristal y diamante solo en lo
//     alto). En las paredes de las cuevas hondas, menas que sobresalen de la roca.
//   - Rocas sueltas (con musgo en el bosque, nieve en el frio, grava alrededor), troncos
//     caidos con setas y arbustos de hojas.
//
// Todo es funcion pura de (semilla, x, y, z) con los hashes del motor (nada de <random> ni
// std::shuffle): el mismo mundo en MSVC, libstdc++ y libc++. Nada de esto pasa dentro de las
// kNaturaRing regiones del origen (VoxelInternal.h): alli el mundo sigue bloque a bloque como
// estaba (las ediciones de los jugadores guardadas por coordenada siguen en su sitio).

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
float rand01(int x, int y, int z, std::uint32_t seed) { return static_cast<float>(hash3(x, y, z, seed) & 0xFFFFFF) / 16777215.0f; }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
int floorDivInt(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
int cheb(int x, int z) { return std::max(std::abs(x), std::abs(z)); }

using AB = Generator::AdventureBiome;

}  // namespace

// --- Interruptor ---------------------------------------------------------------------------

float Generator::natureWeight(float x, float z) const {
    if (!nature_ || profile_ != VoxelGenerator::Adventure) return 0.0f;
    const float edge = static_cast<float>(natureEdge());
    if (edge <= 0.0f) return 1.0f;
    return smoothstep(edge, edge + kNaturaBlend, std::max(std::abs(x), std::abs(z)));
}

bool Generator::natureNew(int x, int z) const {
    return nature_ && profile_ == VoxelGenerator::Adventure && cheb(x, z) >= natureEdge();
}

// --- Relieve -------------------------------------------------------------------------------

float Generator::natureRelief(float x, float z, float land, float inland, float river_noise, float h, AdventureColumn& c) const {
    const float w = natureWeight(x, z);
    if (w <= 0.0f || land <= 0.0f) return h;
    const float sea = static_cast<float>(sea_);
    float out = h;
    // Cordilleras: zonas largas; lejos de la costa (inland) y nunca en el agua.
    const float zone = fbm2(x / 2300.0f, z / 2300.0f, 3, 601) + fbm2(x / 700.0f, z / 700.0f, 2, 602) * 0.18f;
    // Valles: las montanas bajan hacia los rios (el rio corre por el fondo de un valle en V, no por
    // un tajo vertical); `river_noise` es el ruido de los rios (0 en el cauce).
    const float valley = 0.3f + 0.7f * smoothstep(0.02f, 0.13f, river_noise);  // collados entre valle y valle
    // Dentro de las provincias salvajes (cordilleras gigantes, pantanos, mesas...) manda su relieve.
    const float wild = c.region == WNone ? 1.0f : 1.0f - c.strength;
    float mask = smoothstep(0.02f, 0.26f, zone) * w * smoothstep(0.0f, 0.5f, inland) * valley * wild;
    if (mask > 0.0f) {
        // Los desiertos calidos y secos se quedan casi llanos (dunas y mesetas de terracota).
        const float t = fbm2(x / 1200.0f, z / 1200.0f, 3, 107) * 1.7f, hu = fbm2(x / 1000.0f, z / 1000.0f, 3, 108) * 1.7f;
        mask *= 1.0f - 0.8f * smoothstep(0.15f, 0.4f, t) * (1.0f - smoothstep(-0.15f, 0.1f, hu));
    }
    if (mask > 0.0f) {
        // Crestas con el dominio retorcido (no lineas rectas).
        const float wx = x + fbm2(x / 520.0f, z / 520.0f, 2, 603) * 170.0f;
        const float wz = z + fbm2(x / 520.0f + 3.7f, z / 520.0f - 1.3f, 2, 604) * 170.0f;
        const float r1 = 1.0f - std::abs(fbm2(wx / 620.0f, wz / 620.0f, 4, 605));  // crestas largas
        const float r2 = 1.0f - std::abs(fbm2(wx / 180.0f, wz / 180.0f, 3, 606));  // contrafuertes
        const float body = smoothstep(0.45f, 1.0f, r1);                              // macizo (0 en los valles)
        const float spine = body * body * body * body;                               // la cresta, afilada
        float peak = body * 52.0f + spine * 125.0f + r2 * r2 * r2 * 42.0f * body + fbm2(x / 34.0f, z / 34.0f, 2, 607) * 3.0f * body;
        peak *= 0.7f + 0.3f * smoothstep(0.0f, 0.45f, zone);  // mas altas en el corazon de la zona
        const float target = sea + 6.0f + peak;
        out = std::max(out, lerp(out, target, mask));
    }
    // Mesetas: tierra alta de cima llana, escalones de 9 m y laderas casi verticales.
    const float pz = fbm2(x / 1150.0f + 11.3f, z / 1150.0f - 4.1f, 3, 608) + fbm2(x / 240.0f, z / 240.0f, 2, 610) * 0.05f;
    // Las mesetas si dejan que el rio las corte en canon (paredes casi verticales).
    const float pm = smoothstep(0.20f, 0.25f, pz) * w * wild * smoothstep(0.15f, 0.6f, inland) * (1.0f - std::min(1.0f, mask * 2.0f)) *
                     smoothstep(0.035f, 0.07f, river_noise);
    if (pm > 0.0f) {
        float top = sea + 30.0f + std::max(0.0f, fbm2(x / 320.0f, z / 320.0f, 2, 609)) * 50.0f + (pz - 0.2f) * 60.0f;
        const float q = top / 9.0f, fl = std::floor(q);
        top = 9.0f * (fl + smoothstep(0.8f, 1.0f, q - fl));
        out = std::max(out, lerp(out, top, pm));
    }
    // Las cumbres se aplastan al acercarse al techo del mundo (kWorldHeight 256).
    if (out > sea + 150.0f) out = sea + 150.0f + (out - sea - 150.0f) * 0.55f;
    if (out > sea + 120.0f && mask > 0.3f) c.peak = true;
    return out;
}

// --- Afloramientos, rocas, troncos caidos y arbustos ---------------------------------------

void Generator::placeNatureDecor(ChunkData& chunk) const {
    if (!nature_ || profile_ != VoxelGenerator::Adventure) return;
    const int x0 = chunk.cx * kChunkSize, z0 = chunk.cz * kChunkSize;
    constexpr int kCell = 10, kReach = 12;  // kReach >= lo que mide la roca mas grande (ext + 1)
    {
        // Chunk entero dentro del mundo de antes (ni lo que alcanza desde fuera).
        const int far_x = std::max(std::abs(x0 - kReach), std::abs(x0 + kChunkSize + kReach));
        const int far_z = std::max(std::abs(z0 - kReach), std::abs(z0 + kChunkSize + kReach));
        if (std::max(far_x, far_z) < natureEdge()) return;
    }
    const auto s = static_cast<std::uint32_t>(seed_) + 8111u;
    const auto at = [&](int wx, int y, int wz) -> BlockId {
        const int lx = wx - x0, lz = wz - z0;
        if (lx < 0 || lz < 0 || lx >= kChunkSize || lz >= kChunkSize || y < 0 || y >= kWorldHeight) return block::Air;
        return chunk.get(lx, y, lz);
    };
    const auto put = [&](int wx, int y, int wz, BlockId id, bool only_air) {
        const int lx = wx - x0, lz = wz - z0;
        if (lx < 0 || lz < 0 || lx >= kChunkSize || lz >= kChunkSize || y < 1 || y >= kWorldHeight) return;
        BlockId& cell = chunk.blocks[static_cast<std::size_t>(cellIndex(lx, y, lz))];
        if (only_air && cell != block::Air && !blockDef(cell).replaceable) return;
        if (cell == block::Water || cell == block::Lava || cell == block::Bedrock) return;
        cell = id;
    };
    const float clear_margin = treeClearMargin();
    const auto entrances = entrancesTouching(x0 - kReach, z0 - kReach, x0 + kChunkSize + kReach, z0 + kChunkSize + kReach);

    for (int gz = floorDivInt(z0 - kReach, kCell); gz <= floorDivInt(z0 + kChunkSize + kReach, kCell); ++gz) {
        for (int gx = floorDivInt(x0 - kReach, kCell); gx <= floorDivInt(x0 + kChunkSize + kReach, kCell); ++gx) {
            const float roll = rand01(gx, 1, gz, s);
            if (roll > 0.42f) continue;  // lo mas comun: nada (barato)
            const int px = gx * kCell + 2 + static_cast<int>(rand01(gx, 2, gz, s) * (kCell - 4));
            const int pz = gz * kCell + 2 + static_cast<int>(rand01(gx, 3, gz, s) * (kCell - 4));
            if (!natureNew(px, pz)) continue;
            const AdventureColumn c = adventureColumn(px, pz);
            if (c.water > c.height || c.lake || c.river > 0.15f || c.lava > c.height || c.feature != 0 || c.height < sea_ + 1 ||
                c.height >= kWorldHeight - 14 || c.biome > AB::AMountains || c.biome == AB::AOcean || c.biome == AB::ARiver ||
                c.biome == AB::ABeach) {
                continue;
            }
            if (structures_ && structures_->blocksTrees(px, pz, clear_margin)) continue;
            if (!entrances.empty() && nearCaveEntrance(entrances, px, pz, 3.0f)) continue;
            if (caveOpenNear(px, pz)) continue;
            const int base = c.height;
            const bool high = base > sea_ + 45 || c.biome == AB::AMountains;
            // Que hay en esta celda (probabilidades acumuladas sobre `roll` < 0.42).
            float p_ore = 0.0f, p_rock = 0.0f, p_log = 0.0f, p_bush = 0.0f;
            switch (c.biome) {
                case AB::AMountains: p_ore = 0.19f, p_rock = 0.10f; break;
                case AB::ASnow: p_ore = 0.10f, p_rock = 0.08f, p_log = 0.02f; break;
                case AB::AForest: p_ore = 0.04f, p_rock = 0.08f, p_log = 0.07f, p_bush = 0.10f; break;
                case AB::AMeadow: p_ore = 0.035f, p_rock = 0.05f, p_log = 0.01f, p_bush = 0.06f; break;
                case AB::ADesert: p_ore = 0.06f, p_rock = 0.04f; break;
                default: break;
            }
            if (high && c.biome != AB::AMountains) p_ore += 0.10f;
            // Troncos y arbustos son vegetacion: siguen la densidad y los claros del mundo (VoxelWorld).
            p_log = treeChance(p_log, px, pz);
            p_bush = treeChance(p_bush, px, pz);
            const float k = roll;
            int what = -1;  // 0 afloramiento, 1 roca, 2 tronco, 3 arbusto
            if (k < p_ore) what = 0;
            else if (k < p_ore + p_rock) what = 1;
            else if (k < p_ore + p_rock + p_log) what = 2;
            else if (k < p_ore + p_rock + p_log + p_bush) what = 3;
            if (what < 0) continue;
            // Pendiente (a 2 m): los troncos y arbustos solo en llano; las rocas aguantan mas.
            const int hx = adventureColumn(px + 2, pz).height, hz = adventureColumn(px, pz + 2).height;
            const int slope = std::max(std::abs(hx - base), std::abs(hz - base));
            if ((what >= 2 && slope > 1) || slope > 7) continue;
            const auto r01 = [&](int i) { return rand01(gx, 10 + i, gz, s); };

            if (what == 2) {
                // Tronco caido: a lo largo de x o de z, con setas y algun helecho encima.
                const bool along_x = r01(0) < 0.5f;
                const int len = 4 + static_cast<int>(r01(1) * 4.0f);
                const BlockId log = c.biome == AB::ASnow ? block::SpruceLog : (r01(2) < 0.3f ? block::BirchLog : block::OakLog);
                const int end_h = adventureColumn(px + (along_x ? len : 0), pz + (along_x ? 0 : len)).height;
                if (std::abs(end_h - base) > 1) continue;
                for (int i = 0; i < len; ++i) {
                    const int x = px + (along_x ? i : 0), z = pz + (along_x ? 0 : i);
                    put(x, base + 1, z, log, true);
                    const float f = rand01(x, base + 2, z, s + 5u);
                    if (f < 0.22f) put(x, base + 2, z, f < 0.08f ? block::RedMushroom : block::BrownMushroom, true);
                    else if (f < 0.30f) put(x + (along_x ? 0 : 1), base + 1, z + (along_x ? 1 : 0), block::Fern, true);
                }
                continue;
            }
            if (what == 3) {
                // Arbusto: monticulo de hojas con un tocon dentro.
                const BlockId leaves = c.biome == AB::ASnow ? block::SpruceLeaves : (r01(3) < 0.25f ? block::BirchLeaves : block::OakLeaves);
                const float r = 1.2f + r01(4) * 0.9f;
                const int ri = static_cast<int>(std::ceil(r));
                put(px, base + 1, pz, c.biome == AB::ASnow ? block::SpruceLog : block::OakLog, true);
                for (int dy = 0; dy <= 2; ++dy) {
                    for (int dz = -ri; dz <= ri; ++dz) {
                        for (int dx = -ri; dx <= ri; ++dx) {
                            const float d = static_cast<float>(dx * dx + dz * dz) / (r * r) + static_cast<float>(dy * dy) / 2.6f;
                            if (d > 1.0f) continue;
                            if (d > 0.55f && rand01(px + dx, base + dy, pz + dz, s + 7u) < 0.35f) continue;
                            put(px + dx, base + 1 + dy, pz + dz, leaves, true);
                        }
                    }
                }
                continue;
            }

            // Roca (what 1) o afloramiento (what 0): de 1 a 3 lobulos achatados y facetados.
            const bool cold = c.biome == AB::ASnow || base >= sea_ + 70 - static_cast<int>(c.temperature * 15.0f);
            BlockId ore = block::Air;
            float ore_frac = 0.0f;
            int crystal = 0;  // 1 geoda de cristal (agujas que salen), 2 diamante
            BlockId crystal_block = block::CrystalBlue;
            float big = r01(5);
            if (what == 0) {
                // Mineral por bioma y altura.
                const float o = r01(6);
                const bool desert = c.biome == AB::ADesert;
                const float w_coal = desert ? 0.10f : 0.32f;
                const float w_iron = 0.30f;
                const float w_copper = base < sea_ + 90 ? (desert ? 0.30f : 0.18f) : 0.04f;
                const float w_gold = (high || desert) ? 0.14f : 0.0f;
                const float w_crystal = (base > sea_ + 75 || cold) ? 0.12f : 0.0f;
                const float w_diamond = base > sea_ + 110 ? 0.05f : 0.0f;
                const float total = w_coal + w_iron + w_copper + w_gold + w_crystal + w_diamond;
                float t = o * total;
                if ((t -= w_coal) < 0.0f) ore = block::CoalOre;
                else if ((t -= w_iron) < 0.0f) ore = block::IronOre;
                else if ((t -= w_copper) < 0.0f) ore = block::CopperOre;
                else if ((t -= w_gold) < 0.0f) ore = block::GoldOre;
                else if ((t -= w_crystal) < 0.0f) ore = block::CrystalOre, crystal = 1;
                else ore = block::DiamondOre, crystal = 2;
                ore_frac = ore == block::CoalOre || ore == block::IronOre ? 0.45f : (ore == block::GoldOre ? 0.30f : 0.38f);
                if (crystal == 1) {
                    crystal_block = cold ? block::CrystalBlue : (base > sea_ + 100 ? block::CrystalViolet : (desert ? block::CrystalPink : block::CrystalGreen));
                }
                if (high) big = std::min(1.0f, big + 0.35f);  // en la montana, mas grandes
            }
            const BlockId host = c.biome == AB::ADesert && what == 1 ? block::Sandstone : block::Stone;
            const int lobes = 1 + static_cast<int>(big * 2.5f);
            struct Lobe {
                float x, y, z, rx, ry, rz;
            } lb[3];
            const float scale = 1.0f + big * 1.4f;
            for (int i = 0; i < lobes; ++i) {
                const float ox = i == 0 ? 0.0f : (r01(20 + i) - 0.5f) * 3.0f * scale;
                const float oz = i == 0 ? 0.0f : (r01(23 + i) - 0.5f) * 3.0f * scale;
                lb[i].rx = (0.9f + r01(26 + i) * 0.9f) * scale;
                lb[i].rz = (0.9f + r01(29 + i) * 0.9f) * scale;
                lb[i].ry = (1.1f + r01(32 + i) * 0.8f) * scale * (i == 0 ? 1.0f : 0.7f);
                lb[i].x = static_cast<float>(px) + 0.5f + ox;
                lb[i].z = static_cast<float>(pz) + 0.5f + oz;
                lb[i].y = static_cast<float>(base) + 0.5f + lb[i].ry * (i == 0 ? 0.55f : 0.25f);  // asoma: algo enterrada
            }
            const int ext = static_cast<int>(std::ceil(3.6f * scale)) + 1;
            const int top_y = base + static_cast<int>(std::ceil(3.4f * scale)) + 2;
            for (int dz = -ext; dz <= ext; ++dz) {
                for (int dx = -ext; dx <= ext; ++dx) {
                    const int x = px + dx, z = pz + dz;
                    if (x < x0 || z < z0 || x >= x0 + kChunkSize || z >= z0 + kChunkSize) continue;
                    int col_top = -1, col_bottom = kWorldHeight;
                    for (int y = top_y; y >= base - 3; --y) {
                        float best = 9.0f;
                        for (int i = 0; i < lobes; ++i) {
                            const float fx = (static_cast<float>(x) + 0.5f - lb[i].x) / lb[i].rx;
                            const float fy = (static_cast<float>(y) + 0.5f - lb[i].y) / lb[i].ry;
                            const float fz = (static_cast<float>(z) + 0.5f - lb[i].z) / lb[i].rz;
                            // Facetas: norma entre la esferica y la del cubo (rocas con caras, no bolas).
                            const float d = 0.6f * (fx * fx + fy * fy + fz * fz) + 0.4f * std::max({fx * fx, fy * fy, fz * fz});
                            best = std::min(best, d);
                        }
                        if (best > 1.0f) continue;
                        if (best > 0.7f && rand01(x, y, z, s + 11u) < 0.35f) continue;  // borde roto
                        BlockId id = host;
                        if (host == block::Stone && rand01(x, y, z, s + 13u) < 0.14f) id = block::Cobblestone;
                        // Veta en grumos de 2 x 2 x 2 (se lee de lejos).
                        if (ore != block::Air &&
                            rand01(floorDivInt(x, 2) + gx * 7, floorDivInt(y, 2), floorDivInt(z, 2) + gz * 7, s + 17u) < ore_frac) {
                            id = ore;
                        }
                        put(x, y, z, id, y > base);  // por debajo del suelo tambien (que no flote)
                        col_top = std::max(col_top, y);
                        col_bottom = std::min(col_bottom, y);
                    }
                    if (col_top < 0) continue;
                    // Pie: hasta el suelo si la ladera baja (como mucho 8).
                    for (int y = col_bottom - 1; y >= col_bottom - 8 && y > 1; --y) {
                        const BlockId below = at(x, y, z);
                        if (below != block::Air && !blockDef(below).replaceable) break;
                        put(x, y, z, host, false);
                    }
                    // Cara de arriba: musgo en el bosque, nieve en el frio.
                    if (col_top > base && at(x, col_top + 1, z) == block::Air) {
                        const BlockId t = at(x, col_top, z);
                        if (t == host || t == block::Cobblestone) {
                            if (cold && rand01(x, col_top, z, s + 19u) < 0.45f) put(x, col_top, z, block::Snow, false);  // la veta sigue a la vista
                            else if (c.biome == AB::AForest && rand01(x, col_top, z, s + 19u) < 0.6f) put(x, col_top, z, block::Moss, false);
                        }
                    }
                }
            }
            // Grava suelta alrededor de las rocas de la montana (pedrera).
            if (high) {
                for (int dz = -ext - 1; dz <= ext + 1; ++dz) {
                    for (int dx = -ext - 1; dx <= ext + 1; ++dx) {
                        const int x = px + dx, z = pz + dz;
                        if (rand01(x, 3, z, s + 23u) > 0.22f) continue;
                        for (int y = top_y; y >= base - 4; --y) {
                            const BlockId b = at(x, y, z);
                            if (b == block::Air || blockDef(b).replaceable) continue;
                            if ((b == block::Grass || b == block::Stone || b == block::Dirt) && at(x, y + 1, z) == block::Air) put(x, y, z, block::Gravel, false);
                            break;
                        }
                    }
                }
            }
            // Geodas: agujas de cristal que salen de la roca en abanico (dan luz: se ven de noche).
            if (crystal != 0) {
                const int n = crystal == 2 ? 3 : 4 + static_cast<int>(r01(40) * 4.0f);
                for (int i = 0; i < n; ++i) {
                    const float a = r01(41 + i);
                    const int dir = static_cast<int>(a * 8.0f) & 7;
                    static constexpr int kD[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
                    const int len = crystal == 2 ? 1 : (i == 0 ? 4 : 2) + static_cast<int>(r01(50 + i) * 3.5f);
                    int x = px + kD[dir][0] * (i == 0 ? 0 : 1), z = pz + kD[dir][1] * (i == 0 ? 0 : 1);
                    // Desde lo alto de la roca en esa columna (de la forma, no de lo que hay en el chunk:
                    // igual se genere desde este chunk o desde el de al lado).
                    int y = base;
                    for (int yy = top_y; yy > base; --yy) {
                        bool inside = false;
                        for (int l = 0; l < lobes && !inside; ++l) {
                            const float fx = (static_cast<float>(x) + 0.5f - lb[l].x) / lb[l].rx;
                            const float fy = (static_cast<float>(yy) + 0.5f - lb[l].y) / lb[l].ry;
                            const float fz = (static_cast<float>(z) + 0.5f - lb[l].z) / lb[l].rz;
                            inside = 0.6f * (fx * fx + fy * fy + fz * fz) + 0.4f * std::max({fx * fx, fy * fy, fz * fz}) <= 0.7f;
                        }
                        if (inside) {
                            y = yy;
                            break;
                        }
                    }
                    const BlockId spike = crystal == 2 ? block::CrystalSprout : crystal_block;
                    if (crystal == 2) ++y;  // el brote va encima
                    for (int j = 0; j < len; ++j) {
                        put(x, y + j, z, spike, j > 0 || crystal == 2);
                        if (i != 0 && j == len / 2) {  // se inclina hacia fuera
                            x += kD[dir][0];
                            z += kD[dir][1];
                        }
                    }
                }
            }
        }
    }

    // Menas que sobresalen de las paredes de las cuevas hondas (lejos del origen).
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize - 1; ++x) {
            const int wx = x0 + x, wz = z0 + z;
            if (!natureNew(wx, wz)) continue;
            for (int y = 8; y < 72; ++y) {
                if (chunk.get(x, y, z) != block::Air) continue;
                const BlockId wall = chunk.get(x + 1, y, z);
                if (wall != block::Stone && wall != block::Deepstone) continue;
                const float r = rand01(wx, y, wz, s + 31u);
                if (r > 0.010f) continue;
                BlockId ore = block::CoalOre;
                const float q = r / 0.010f;
                if (y < 20) ore = q < 0.25f ? block::DiamondOre : (q < 0.6f ? block::CrystalOre : block::GoldOre);
                else if (y < 36) ore = q < 0.35f ? block::GoldOre : (q < 0.5f ? block::CrystalViolet : block::IronOre);
                else ore = q < 0.45f ? block::IronOre : (q < 0.7f ? block::CopperOre : block::CoalOre);
                chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))] = ore;
                if (q > 0.5f && chunk.get(x, y + 1, z) == block::Air) chunk.blocks[static_cast<std::size_t>(cellIndex(x, y + 1, z))] = ore;
                if (q < 0.3f && x > 0 && chunk.get(x - 1, y, z) == block::Air && chunk.get(x - 1, y - 1, z) != block::Air) {
                    chunk.blocks[static_cast<std::size_t>(cellIndex(x - 1, y, z))] = ore;  // sobresale 2
                }
            }
        }
    }
}

}  // namespace cramion::voxel
