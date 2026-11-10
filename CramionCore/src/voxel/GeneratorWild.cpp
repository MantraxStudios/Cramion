// Tierras salvajes del generador Aventura: lo que hay lejos del origen. Cerca
// del origen (menos de ~1,4 km) el mundo es exactamente el de siempre; de ahi
// en adelante el mundo se reparte en provincias (celdas de Voronoi de ~1,6 km)
// y cada una puede ser:
//
//   - Cordillera gigante: crestas afiladas de 150-180 m sobre el mar, cumbres
//     nevadas con paredes de roca desnuda (se ven desde kilometros).
//   - Archipielago flotante: 2-5 islas grandes en el cielo (90-250 m de ancho,
//     cima a 160-235 m) con satelites medianos: bosque, pradera con lago, picos
//     o ruinas arriba; acantilados en terrazas y una montana invertida con agujas
//     debajo; cuevas por dentro (gruta de cristal, lago, bocas al cielo) y
//     cascadas que salen de los lagos. Tambien hay alguna isla suelta sobre las
//     cordilleras y, muy de vez en cuando, en cualquier sitio salvaje.
//   - Tierras Carmesi: arena roja, arenisca roja y terracota; mesetas por
//     escalones de 8 m y chimeneas de hadas.
//   - Desierto Rosa: dunas de arena rosa y agujas inclinadas de cuarzo rosa con
//     la punta de cristal (dan luz de noche).
//   - Paramo de Ceniza: basalto y ceniza, volcanes de 70-130 m con un lago de
//     lava en el crater, charcas de lava con costras de azufre, respiraderos de
//     magma y arboles calcinados.
//   - Tierras Marchitas: hierba marchita violeta, espinas, huesos, arboles
//     muertos, puas de obsidiana y racimos de cristal violeta.
//   - Nada: tierra normal (la de siempre).
//
// Todo es funcion pura de (semilla, x, z) como el resto del generador: el
// relieve va en adventureColumn (lo ven terrainHeight, las estructuras y el
// terreno lejano) y lo suelto (islas, agujas, respiraderos) se escribe por
// chunk probando las celdas vecinas, sin cortes entre chunks.

#include "VoxelInternal.h"

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

constexpr float kWildNear = 1400.0f;  // hasta aqui, el mundo de siempre
constexpr float kWildFar = 2200.0f;   // desde aqui, provincias a pleno
constexpr int kRegionCell = 1600;

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

// Aguja inclinada: de la base en la direccion (dx, dy, dz) (unitaria), con el
// radio de r0 a casi nada; el ultimo `tip` (0..1) de otro bloque.
void spire(Put& w, float bx, float by, float bz, float dx, float dy, float dz, float length, float r0, BlockId id, BlockId tip_id,
           float tip) {
    const float minx = static_cast<float>(w.x0) - r0 - 1.0f, maxx = static_cast<float>(w.x0 + kChunkSize) + r0 + 1.0f;
    const float minz = static_cast<float>(w.z0) - r0 - 1.0f, maxz = static_cast<float>(w.z0 + kChunkSize) + r0 + 1.0f;
    for (float s = 0.0f; s <= length; s += 0.5f) {
        const float k = s / length;
        const float radius = r0 * (1.0f - k) + 0.45f;
        const float px = bx + dx * s, py = by + dy * s, pz = bz + dz * s;
        if (px < minx || px > maxx || pz < minz || pz > maxz) continue;  // lejos de este chunk
        const BlockId here = k > 1.0f - tip ? tip_id : id;
        const int ir = static_cast<int>(std::ceil(radius));
        for (int oy = -ir; oy <= ir; ++oy) {
            for (int oz = -ir; oz <= ir; ++oz) {
                for (int ox = -ir; ox <= ir; ++ox) {
                    if (static_cast<float>(ox * ox + oy * oy + oz * oz) > radius * radius) continue;
                    w.put(ifloor(px) + ox, ifloor(py) + oy, ifloor(pz) + oz, here, false);
                }
            }
        }
    }
}

// Copa redonda con el borde a medias.
void canopy(Put& w, int cx, int cy, int cz, float radius, float radius_y, BlockId leaves, std::uint32_t seed) {
    const int ri = static_cast<int>(std::ceil(radius));
    const int ry = static_cast<int>(std::ceil(radius_y));
    for (int dy = -ry; dy <= ry; ++dy) {
        for (int dz = -ri; dz <= ri; ++dz) {
            for (int dx = -ri; dx <= ri; ++dx) {
                const float d = static_cast<float>(dx * dx + dz * dz) / (radius * radius) + static_cast<float>(dy * dy) / (radius_y * radius_y);
                if (d > 1.0f) continue;
                if (d > 0.7f && rand01(cx + dx, cy + dy, cz + dz, seed) < 0.35f) continue;
                w.put(cx + dx, cy + dy, cz + dz, leaves, true);
            }
        }
    }
}

}  // namespace

// --- Provincias --------------------------------------------------------------------------

Generator::WildCell Generator::wildRegion(float x, float z) const {
    WildCell out;
    const float dist = std::sqrt(x * x + z * z);
    if (dist <= kWildNear) return out;
    const float wild = smoothstep(kWildNear, kWildFar, dist);
    const auto s = static_cast<std::uint32_t>(seed_) + 9101u;
    // OCEANOS-HZ: con los continentes grandes la costa ya no recorta las provincias y su borde
    // (recto, de Voronoi) se veia; fuera de las regiones del origen, el borde serpentea.
    if (const float ow = oceanScaleWeight(x, z); ow > 0.0f) {
        const float qx = x, qz = z;
        x += oceanFbm(qx / 700.0f, qz / 700.0f, 3, 31) * 260.0f * ow;
        z += oceanFbm(qx / 700.0f, qz / 700.0f, 3, 32) * 260.0f * ow;
    }
    const float cell = static_cast<float>(kRegionCell);
    const int gx = floorDiv(ifloor(x), kRegionCell), gz = floorDiv(ifloor(z), kRegionCell);
    float d1 = 1e9f, d2 = 1e9f;
    int bx = 0, bz = 0;
    float bcx = 0.0f, bcz = 0.0f;
    for (int oz = -1; oz <= 1; ++oz) {
        for (int ox = -1; ox <= 1; ++ox) {
            const int cx = gx + ox, cz = gz + oz;
            const float px = (static_cast<float>(cx) + 0.15f + 0.7f * rand01(cx, 1, cz, s)) * cell;
            const float pz = (static_cast<float>(cz) + 0.15f + 0.7f * rand01(cx, 2, cz, s)) * cell;
            const float d = std::hypot(x - px, z - pz);
            if (d < d1) {
                d2 = d1;
                d1 = d;
                bx = cx;
                bz = cz;
                bcx = px;
                bcz = pz;
            } else if (d < d2) {
                d2 = d;
            }
        }
    }
    const float roll = rand01(bx, 3, bz, s);
    int region = WNone;
    if (roll < 0.18f) region = WPeaks;
    else if (roll < 0.31f) region = WSky;
    else if (roll < 0.44f) region = WBadlands;
    else if (roll < 0.56f) region = WRose;
    else if (roll < 0.69f) region = WAsh;
    else if (roll < 0.81f) region = WBlight;  // el resto (~19 %): tierra normal
    // Clima del centro de la provincia: sin desiertos en el frio.
    if (region == WBadlands || region == WRose) {
        const float temperature = fbm2(bcx / 1200.0f, bcz / 1200.0f, 3, 107) * 1.7f;
        if (temperature < -0.25f) region = region == WBadlands ? WPeaks : WBlight;
    }
    // NOCHE-MUNDO: lo que era tierra normal pasa a ser uno de los biomas frondosos (GeneratorLush.cpp),
    // con un poco de criterio por el clima del centro: ni selva ni sabana en el frio.
    if (region == WNone && lush_biomes_) {
        const float k = std::clamp((roll - 0.81f) / 0.19f, 0.0f, 0.9999f);
        region = WJungle + static_cast<int>(k * 5.0f);
        if (region == WJungle || region == WSavanna) {
            const float temperature = fbm2(bcx / 1200.0f, bcz / 1200.0f, 3, 107) * 1.7f;
            if (temperature < -0.05f) region = region == WJungle ? WAutumn : WBlossom;
        }
    }
    out.region = region;
    out.cx = bcx;
    out.cz = bcz;
    // Distancia al borde con la provincia de al lado ~ (d2 - d1) / 2.
    out.strength = smoothstep(60.0f, 300.0f, (d2 - d1) * 0.5f) * wild;
    return out;
}

void Generator::wildShape(float x, float z, float land, float inland, int stage, float& h, AdventureColumn& c) const {
    const float sea = static_cast<float>(sea_);
    if (stage == 0) {
        const WildCell w = wildRegion(x, z);
        c.region = w.region;
        c.strength = w.strength;
        if (w.region != WPeaks || w.strength <= 0.0f) return;
        // Cordillera gigante: crestas ("ridged") grandes con otras mas pequenas encima.
        const float m = smoothstep(0.0f, 1.0f, w.strength * std::clamp(land * 3.0f, 0.0f, 1.0f));
        if (m <= 0.0f) return;
        const float r1 = 1.0f - std::abs(fbm2(x / 640.0f, z / 640.0f, 4, 210));
        const float r2 = 1.0f - std::abs(fbm2(x / 200.0f, z / 200.0f, 3, 211));
        const float mount = r1 * r1 * 125.0f + r2 * r2 * 40.0f * r1 + fbm2(x / 50.0f, z / 50.0f, 2, 212) * 4.0f;
        const float target = sea + 14.0f + mount * (0.35f + 0.65f * inland);
        h = std::max(h, lerp(h, target, m));
        c.giant = m > 0.35f && h > sea + 90.0f;
        return;
    }
    if (c.region < WBadlands || c.strength <= 0.0f) return;
    const float k = smoothstep(0.0f, 1.0f, c.strength * std::clamp(land * 4.0f, 0.0f, 1.0f));
    if (k <= 0.0f) return;
    const auto s = static_cast<std::uint32_t>(seed_) + 9203u;

    if (stage == 1) {
        switch (c.region) {
            case WBadlands: {
                float base = sea + 7.0f + land * 8.0f + fbm2(x / 150.0f, z / 150.0f, 3, 221) * 7.0f;
                // Mesetas por escalones de 8 m (paredes verticales entre escalon y escalon).
                const float mn = fbm2(x / 190.0f, z / 190.0f, 3, 220) + fbm2(x / 60.0f, z / 60.0f, 2, 222) * 0.08f;
                const bool on_mesa = mn > 0.02f;
                if (on_mesa) {
                    const float t = std::min((mn - 0.02f) * 260.0f, 72.0f);
                    base = std::max(base, sea + 12.0f + std::floor(t / 8.0f) * 8.0f);
                }
                h = lerp(h, base, k);
                // Chimeneas de hadas en el llano.
                if (!on_mesa && k > 0.5f) {
                    constexpr int kCell = 28;
                    const int gx = floorDiv(ifloor(x), kCell), gz = floorDiv(ifloor(z), kCell);
                    float best = 0.0f;
                    for (int oz = -1; oz <= 1; ++oz) {
                        for (int ox = -1; ox <= 1; ++ox) {
                            const int cx = gx + ox, cz = gz + oz;
                            if (rand01(cx, 0, cz, s) > 0.35f) continue;
                            const float px = (static_cast<float>(cx) + 0.2f + 0.6f * rand01(cx, 1, cz, s)) * kCell;
                            const float pz = (static_cast<float>(cz) + 0.2f + 0.6f * rand01(cx, 2, cz, s)) * kCell;
                            const float radius = 1.6f + rand01(cx, 3, cz, s) * 2.2f;
                            const float d = std::hypot(x + 0.5f - px, z + 0.5f - pz);
                            if (d >= radius) continue;
                            const float q = d / radius;
                            best = std::max(best, (8.0f + rand01(cx, 4, cz, s) * 22.0f) * (1.0f - q * q * q * q * 0.35f));
                        }
                    }
                    if (best > 0.0f) {
                        c.feature = 1;
                        c.feature_base = static_cast<int>(std::lround(h));
                        h += best;
                    }
                }
                break;
            }
            case WRose: {
                // Dunas: crestas largas ("ridged" estirado) y rizos.
                const float ridge = 1.0f - std::abs(fbm2(x / 120.0f, z / 75.0f, 3, 230));
                const float dunes = ridge * ridge * ridge * 22.0f + fbm2(x / 30.0f, z / 30.0f, 2, 231) * 1.5f;
                h = lerp(h, sea + 5.0f + land * 6.0f + dunes, k);
                break;
            }
            case WAsh: {
                const float base = sea + 8.0f + land * 10.0f + fbm2(x / 140.0f, z / 140.0f, 4, 240) * 14.0f;
                // Volcanes: conos con un crater de lava, uno como mucho por celda de 480 m.
                constexpr int kCell = 480;
                const int gx = floorDiv(ifloor(x), kCell), gz = floorDiv(ifloor(z), kCell);
                float volcano = 0.0f;
                int lava = 0;
                for (int oz = -1; oz <= 1; ++oz) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        const int cx = gx + ox, cz = gz + oz;
                        if (rand01(cx, 10, cz, s) > 0.7f) continue;
                        const float px = (static_cast<float>(cx) + 0.3f + 0.4f * rand01(cx, 11, cz, s)) * kCell;
                        const float pz = (static_cast<float>(cz) + 0.3f + 0.4f * rand01(cx, 12, cz, s)) * kCell;
                        const float R = 100.0f + rand01(cx, 13, cz, s) * 90.0f;
                        const float d = std::hypot(x - px, z - pz);
                        if (d >= R) continue;
                        const float H = 70.0f + rand01(cx, 14, cz, s) * 60.0f;
                        const float rc = R * 0.16f;
                        const float rim = H * std::pow(1.0f - rc / R, 1.6f);
                        float cone;
                        if (d < rc) {
                            cone = rim - 14.0f;  // el fondo del crater
                            lava = std::max(lava, static_cast<int>(std::lround(base + cone)) + 5);
                        } else {
                            cone = H * std::pow(1.0f - d / R, 1.6f) + fbm2(x / 25.0f, z / 25.0f, 2, 243) * 3.0f * (1.0f - d / R);
                        }
                        volcano = std::max(volcano, cone);
                    }
                }
                h = lerp(h, base + volcano, k);
                if (lava > 0 && k > 0.8f) {
                    c.lava = lava;
                    c.feature = 3;
                }
                break;
            }
            case WBlight: {
                const float base = sea + 5.0f + land * 8.0f + fbm2(x / 130.0f, z / 130.0f, 3, 250) * 9.0f;
                h = lerp(h, base, k * 0.85f);
                // Puas de obsidiana.
                if (k > 0.5f) {
                    constexpr int kCell = 22;
                    const int gx = floorDiv(ifloor(x), kCell), gz = floorDiv(ifloor(z), kCell);
                    float best = 0.0f;
                    for (int oz = -1; oz <= 1; ++oz) {
                        for (int ox = -1; ox <= 1; ++ox) {
                            const int cx = gx + ox, cz = gz + oz;
                            if (rand01(cx, 20, cz, s) > 0.28f) continue;
                            const float px = (static_cast<float>(cx) + 0.2f + 0.6f * rand01(cx, 21, cz, s)) * kCell;
                            const float pz = (static_cast<float>(cz) + 0.2f + 0.6f * rand01(cx, 22, cz, s)) * kCell;
                            const float radius = 1.3f + rand01(cx, 23, cz, s) * 1.9f;
                            const float d = std::hypot(x + 0.5f - px, z + 0.5f - pz);
                            if (d >= radius) continue;
                            best = std::max(best, (6.0f + rand01(cx, 24, cz, s) * 16.0f) * (1.0f - d / radius));
                        }
                    }
                    if (best >= 1.0f) {
                        c.feature = 2;
                        c.feature_base = static_cast<int>(std::lround(h));
                        h += best;
                    }
                }
                break;
            }
            default:
                if (isLushRegion(c.region)) lushShape(x, z, land, 1, k, h, c);  // NOCHE-MUNDO (GeneratorLush.cpp)
                break;
        }
        return;
    }

    // NOCHE-MUNDO: charcas del pantano y estanques de los cerezos (despues de los rios).
    if (isLushRegion(c.region)) {
        lushShape(x, z, land, 2, k, h, c);
        return;
    }
    // stage 2 (despues de los rios): charcas de lava en el paramo de ceniza.
    if (c.region != WAsh) return;
    if (c.river > 0.01f) {
        c.lava = 0;  // un rio atraviesa el crater: sin lava (no se sale)
        if (c.feature == 3) c.feature = 0;
        return;
    }
    if (c.lava > 0) return;  // crater
    if (k < 0.9f) return;
    const float lp = fbm2(x / 48.0f, z / 48.0f, 2, 244);
    if (lp < 0.30f) return;
    // El nivel: el suelo suave de alrededor (sin el detalle), siempre sobre el mar.
    const float hb = std::max(sea + 4.0f, sea + 8.0f + land * 10.0f + fbm2(x / 140.0f, z / 140.0f, 4, 240) * 14.0f);
    const int level = static_cast<int>(std::lround(hb));  // primer bloque por encima de la lava
    if (lp < 0.36f) {
        h = std::max(h, static_cast<float>(level));  // orilla: contiene la lava
    } else {
        h = std::min(h, static_cast<float>(level) - 2.0f - std::floor(smoothstep(0.36f, 0.5f, lp) * 3.0f));
        c.lava = level;
    }
}

int Generator::wildBiome(const AdventureColumn& c) const {
    switch (c.region) {
        case WBadlands: return AB::ABadlands;
        case WRose: return AB::ARoseDesert;
        case WAsh: return AB::AAshlands;
        case WBlight: return AB::ABlight;
        // NOCHE-MUNDO: biomas frondosos.
        case WJungle: return AB::AJungle;
        case WBlossom: return AB::ABlossom;
        case WSwamp: return AB::ASwamp;
        case WSavanna: return AB::ASavanna;
        case WAutumn: return AB::AAutumn;
        default: return c.biome;
    }
}

BlockId Generator::wildBlock(const AdventureColumn& c, int x, int y, int z, int depth, int soil, bool cliff, bool underwater) const {
    const float fx = static_cast<float>(x), fz = static_cast<float>(z);
    // Capas de las Tierras Carmesi (como las de la meseta del desierto, con arenisca roja).
    const auto band = [&]() {
        const int b = (y + static_cast<int>(fbm2(fx / 40.0f, fz / 40.0f, 1, 122) * 4.0f) + 72) % 9;
        return b < 3 ? block::Terracotta : (b < 5 ? block::RedSandstone : (b == 5 ? block::RedSand : (b < 8 ? block::Terracotta : block::RedSandstone)));
    };
    // Chimeneas de hadas y puas: todo el relieve suelto de un material.
    if ((c.feature == 1 || c.feature == 2) && y > c.feature_base) {
        if (c.feature == 2) return block::Obsidian;
        return depth == 0 ? block::RedSandstone : band();
    }
    if (isLushBiome(c.biome)) return lushBlock(c, x, y, z, depth, soil, cliff, underwater);  // NOCHE-MUNDO
    const auto seed = static_cast<std::uint32_t>(seed_) + 9411u;
    switch (c.biome) {
        case AB::ABadlands:
            if (depth < 3) return cliff ? band() : block::RedSand;
            if (y > sea_ + 2) return band();
            return depth < soil + 6 ? block::RedSandstone : block::Air;
        case AB::ARoseDesert:
            if (depth == 0) return cliff ? block::PinkSandstone : block::PinkSand;
            if (depth < 3) return cliff ? block::PinkSandstone : block::PinkSand;
            return depth < soil + 5 ? block::PinkSandstone : block::Air;
        case AB::AAshlands: {
            if (depth == 0 && c.lava > c.height) return block::Magma;  // el fondo de la lava
            if (depth < 2) {
                if (cliff) return block::Basalt;
                return fbm2(fx / 20.0f, fz / 20.0f, 2, 260) > 0.05f ? block::Ash : block::Basalt;
            }
            if (depth < 12) return rand01(x, y, z, seed) < 0.025f ? block::Magma : block::Basalt;
            return block::Air;
        }
        case AB::ABlight:
            if (depth == 0) return cliff ? block::DuskStone : (underwater ? block::DuskSoil : block::BlightGrass);
            if (depth < soil) return cliff ? block::DuskStone : block::DuskSoil;
            return depth < soil + 3 ? block::DuskStone : block::Air;
        default: return block::Air;
    }
}

// --- Islas flotantes ----------------------------------------------------------------------
//
// Pocas y grandes. Una rejilla de celdas de 560 m: en las provincias de
// archipielago mas o menos la mitad de las celdas tiene una isla grande
// (90-250 m) con 0-3 satelites medianos; sobre las cordilleras, alguna; en el
// resto de lo salvaje, muy pocas (una cada varias provincias). Los grupos que
// se tocarian se descartan (gana el de mas prioridad), asi que no se pisan.
//
// Cada isla: costa organica (ruido con el dominio deformado, con lobulos y
// bahias), cima con colinas y terrazas de 5 m hacia la costa, un borde de roca
// de 9-20 m y debajo una montana invertida (crestas) con agujas de roca.
// Tipos: bosque, pradera con lago, picos de roca (con nieve arriba) y ruinas.
// Dentro: una gruta de cristal, una camara con un lago, bocas que se abren en
// el acantilado y la panza (se ve el cielo) y tuneles entre ellas. Las
// cascadas salen de un lago o un manantial de la cima y caen por el borde.

namespace {

constexpr int kIslandCell = 560;
constexpr float kIslandBound = 1.55f;  // la isla cabe en r * esto desde su centro
constexpr int kIslandReach = 330;      // un grupo (isla + satelites) cabe en esto desde su centro
constexpr int kIslandCeil = kWorldHeight - 4;

struct PreIsland {
    float cx = 0.0f, cz = 0.0f, r = 0.0f, cluster = 0.0f;
    int sats = 0;
    float sat_x[3]{}, sat_z[3]{}, sat_r[3]{};
    std::uint32_t prio = 0;
    bool sky = false;
};

// Cache por hilo de las islas por celda (las columnas seguidas piden las mismas).
struct IslandCellSlot {
    std::uint64_t owner = 0;
    int gx = 0, gz = 0, n = -1;
    std::array<Generator::WildIsland, 4> isl{};
};

}  // namespace

bool Generator::wildIslandColumn(const WildIsland& isl, int x, int z, WildIslandColumn& out) const {
    const float fx = static_cast<float>(x) + 0.5f, fz = static_cast<float>(z) + 0.5f;
    const float r = isl.r;
    const float ddx = fx - isl.cx, ddz = fz - isl.cz;
    const float bound = r * kIslandBound;
    if (ddx * ddx + ddz * ddz > bound * bound) return false;
    const int salt = 840 + static_cast<int>(isl.id % 11u) * 9;
    // Costa: el dominio deformado y lobulos (peninsulas y bahias).
    const float ws = r * 0.55f;
    const float qx = fx + fbm2(fx / ws, fz / ws, 2, salt) * r * 0.32f;
    const float qz = fz + fbm2(fx / ws + 37.1f, fz / ws - 11.7f, 2, salt + 1) * r * 0.32f;
    const float lobes = fbm2(qx / (r * 0.42f), qz / (r * 0.42f), 3, salt + 2);
    const float t = std::hypot(qx - isl.cx, qz - isl.cz) / r * (1.0f - 0.5f * lobes);
    if (t >= 1.0f) return false;

    // Cima: cupula suave, colinas y (picos) crestas de roca.
    static constexpr float kHills[4] = {6.0f, 3.5f, 5.0f, 3.0f};
    float top = isl.top + (1.0f - t * t) * r * 0.07f + fbm2(fx / 34.0f, fz / 34.0f, 3, salt + 3) * kHills[isl.kind & 3] * 2.0f;
    bool rock = false;
    if (isl.kind == 2) {
        const float rg = 1.0f - std::abs(fbm2(fx / (r * 0.3f), fz / (r * 0.3f), 3, salt + 4));
        const float peak = rg * rg * rg * 44.0f * (1.0f - smoothstep(0.35f, 0.92f, t));
        top += peak;
        rock = peak > 10.0f;
    }
    // Terrazas: escalones de 5 m hacia la costa.
    // (no por toda la costa igual: una mascara grande decide donde hay mas escalones)
    const float mask = 0.45f + std::clamp(fbm2(fx / 70.0f, fz / 70.0f, 2, salt + 10) + 0.5f, 0.0f, 1.0f);
    const float e = smoothstep(0.5f, 1.0f, t) * 19.0f * mask + fbm2(fx / 18.0f, fz / 18.0f, 2, salt + 5) * 8.0f;
    top -= std::max(0.0f, std::floor(e / 5.0f)) * 5.0f;
    // Ruinas: el suelo allanado.
    if (isl.ruin_r > 0.0f) {
        const float dr = std::hypot(fx - isl.ruin_x, fz - isl.ruin_z);
        top = lerp(static_cast<float>(isl.ruin_level), top, smoothstep(isl.ruin_r, isl.ruin_r + 10.0f, dr));
    }
    // Lago (o manantial): el fondo en cuenco y la orilla siempre por encima del agua.
    int water = 0;
    if (isl.lake_r > 0.0f) {
        float dl = std::hypot(fx - isl.lake_x, fz - isl.lake_z) / isl.lake_r;
        dl *= 1.0f + 0.35f * fbm2(fx / 10.0f, fz / 10.0f, 2, salt + 6);
        const float level = static_cast<float>(isl.lake_level);
        if (dl < 1.0f) {
            top = level - 1.0f - std::floor((1.0f - dl * dl) * isl.lake_depth);
            water = isl.lake_level;
            rock = false;
        } else if (dl < 2.4f) {
            top = lerp(level + 1.0f + (dl - 1.0f) * 2.0f, top, smoothstep(1.15f, 2.4f, dl));
            rock = rock && dl > 1.8f;
        }
    }
    // Panza: el borde de roca y la montana invertida (crestas) con agujas.
    const float rg2 = 1.0f - std::abs(fbm2(fx / (r * 0.28f), fz / (r * 0.28f), 2, salt + 7));
    const float slab = isl.slab * (0.6f + 0.8f * std::clamp(fbm2(fx / 40.0f, fz / 40.0f, 2, salt + 11) + 0.5f, 0.0f, 1.0f));
    const float under = slab + isl.depth * std::pow(1.0f - t, 1.5f) * (0.55f + 0.9f * rg2 * rg2);
    const float sp = fbm2(fx / 7.0f, fz / 7.0f, 2, salt + 8);
    const float spikes = std::pow(std::max(0.0f, sp - 0.04f), 1.4f) * 55.0f * std::pow(1.0f - t, 0.35f);
    // El acantilado del borde siempre con su grosor, tambien bajo las terrazas.
    const float bottom = std::min(isl.top - under - spikes + fbm2(fx / 11.0f, fz / 11.0f, 2, salt + 9) * 4.0f, top - slab * 0.85f - spikes * 0.3f);

    out.top = std::clamp(static_cast<int>(std::lround(top)), 2, kIslandCeil);
    out.bottom = std::clamp(std::min(static_cast<int>(std::lround(bottom)), out.top - 4), 1, out.top);
    out.water = water > out.top ? water : 0;
    out.t = t;
    out.rock = rock;
    return true;
}

bool Generator::wildIslandSpan(const WildIsland& isl, int x, int z, int& bottom, int& top, float& t) const {
    WildIslandColumn c;
    if (!wildIslandColumn(isl, x, z, c)) return false;
    bottom = c.bottom;
    top = std::max(c.top, c.water);
    t = c.t;
    return true;
}

int Generator::wildIslandCell(int gx, int gz, WildIsland* out) const {
    thread_local std::array<IslandCellSlot, 128> cache;
    const std::uint64_t owner = lakes_ ? lakes_->id : 0;
    IslandCellSlot& slot = cache[hash3(gx, 5, gz, 77u) & 127u];
    if (owner != 0 && slot.owner == owner && slot.gx == gx && slot.gz == gz && slot.n >= 0) {
        for (int i = 0; i < slot.n; ++i) out[i] = slot.isl[static_cast<std::size_t>(i)];
        return slot.n;
    }
    const std::uint32_t s = static_cast<std::uint32_t>(seed_) + 811u;
    const float cell = static_cast<float>(kIslandCell);
    constexpr float kTau = 6.2831853f;

    // El esbozo de una celda (sin mirar a las vecinas): si hay isla, donde y de que tamano.
    const auto pre = [&](int cx, int cz, PreIsland& p) {
        p = PreIsland{};
        p.cx = (static_cast<float>(cx) + 0.2f + 0.6f * rand01(cx, 2, cz, s)) * cell;
        p.cz = (static_cast<float>(cz) + 0.2f + 0.6f * rand01(cx, 3, cz, s)) * cell;
        const float dist = std::hypot(p.cx, p.cz);
        if (dist < kWildNear + 150.0f) return false;
        const WildCell w = wildRegion(p.cx, p.cz);
        const float wild = smoothstep(kWildNear, kWildFar, dist);
        float density = 0.03f * wild;  // alguna suelta en cualquier sitio salvaje
        p.sky = w.region == WSky && w.strength > 0.05f;
        if (p.sky) density = std::max(density, 0.62f * smoothstep(0.05f, 0.45f, w.strength));
        else if (w.region == WPeaks) density = std::max(density, 0.16f * w.strength);
        if (rand01(cx, 0, cz, s) >= density) return false;
        const float k = rand01(cx, 1, cz, s);
        p.r = p.sky ? 45.0f + std::pow(k, 1.3f) * 80.0f : 40.0f + k * 50.0f;
        p.cluster = p.r * kIslandBound;
        if (p.r <= 100.0f) {
            const float sr = rand01(cx, 30, cz, s);
            p.sats = p.sky ? std::min(3, static_cast<int>(sr * 3.4f)) : (sr < 0.35f ? 1 : 0);
        }
        const float a0 = rand01(cx, 31, cz, s) * kTau;
        for (int i = 0; i < p.sats; ++i) {
            const float rs = 14.0f + rand01(cx, 32 + i, cz, s) * 20.0f;
            const float a = a0 + static_cast<float>(i) * kTau / static_cast<float>(p.sats) + (rand01(cx, 35 + i, cz, s) - 0.5f) * 0.7f;
            const float d = p.r * kIslandBound + 12.0f + rand01(cx, 38 + i, cz, s) * 28.0f + rs * kIslandBound;
            p.sat_x[i] = p.cx + std::cos(a) * d;
            p.sat_z[i] = p.cz + std::sin(a) * d;
            p.sat_r[i] = rs;
            p.cluster = std::max(p.cluster, d + rs * kIslandBound);
        }
        if (dist - p.cluster < kWildNear) return false;  // nada cerca del origen
        p.prio = hash3(cx, 40, cz, s);
        return true;
    };

    // Una isla entera (forma, tipo, lago, ruinas y cuevas). false: no cabe sobre el suelo.
    const auto build = [&](WildIsland& isl, float cx, float cz, float r, bool sat, int salt, float parent_top) {
        const auto R = [&](int k) { return rand01(gx, salt + k, gz, s); };
        isl = WildIsland{};
        isl.cx = cx;
        isl.cz = cz;
        isl.r = r;
        isl.satellite = sat;
        isl.id = hash3(gx, salt, gz, s);
        const float kr = R(1);
        if (!sat) isl.kind = static_cast<std::uint8_t>(kr < 0.34f ? 0 : (kr < 0.62f ? 1 : (kr < 0.82f ? 2 : 3)));
        else isl.kind = static_cast<std::uint8_t>(kr < 0.45f ? 0 : (kr < 0.75f ? 1 : 2));
        isl.golden = R(2) < 0.15f;
        isl.slab = sat ? 8.0f + R(3) * 6.0f : 13.0f + R(3) * 7.0f;
        isl.depth = sat ? 16.0f + R(4) * 14.0f : 38.0f + R(4) * 27.0f;
        const float cap = isl.kind == 2 ? 194.0f : 220.0f;
        isl.top = sat ? parent_top + (R(5) - 0.6f) * 45.0f : 178.0f + R(5) * 34.0f;
        isl.top = std::clamp(isl.top, 150.0f, cap);
        // Que la panza quede bien por encima del suelo (y de sus arboles): cada
        // muestra del suelo limita lo honda que puede ser la panza ahi (la punta
        // de la montana invertida esta en el centro; hacia el borde es poca cosa).
        float ground_g[17], ground_f[17];
        ground_g[0] = static_cast<float>(std::max(sea_, adventureColumn(ifloor(cx), ifloor(cz)).height));
        ground_f[0] = 1.0f;
        for (int i = 0; i < 16; ++i) {
            const float a = static_cast<float>(i) * kTau / 16.0f + R(6) * 0.4f;
            const float f = (i & 1) != 0 ? 1.0f : 0.5f;
            ground_g[i + 1] = static_cast<float>(std::max(sea_, adventureColumn(ifloor(cx + std::cos(a) * r * f), ifloor(cz + std::sin(a) * r * f)).height));
            ground_f[i + 1] = std::pow(std::max(0.0f, 1.0f - f * 0.8f), 1.5f);  // cuanto de la panza llega ahi
        }
        const auto fits = [&]() {
            for (int i = 0; i < 17; ++i) {
                if (isl.top - isl.slab * 1.4f - isl.depth * 1.45f * ground_f[i] - 22.0f * std::max(ground_f[i], 0.5f) - 20.0f < ground_g[i]) return false;
            }
            return true;
        };
        // Primero subir la isla, luego hacer la panza menos honda; si no cabe, nada.
        while (!fits() && isl.top < cap) isl.top = std::min(cap, isl.top + 2.0f);
        while (!fits() && isl.depth > 18.0f) isl.depth -= 2.0f;
        if (!fits()) return false;
        WildIslandColumn c;
        // Lago (pradera) o manantial: de ahi salen las cascadas.
        float lake_angle = R(7) * kTau;
        const bool lake = isl.kind == 1 ? (!sat || R(8) < 0.5f) : (!sat && R(8) < (isl.kind == 2 ? 0.3f : 0.35f));
        if (lake) {
            const float ld = (0.08f + R(9) * 0.17f) * r;
            isl.lake_x = cx + std::cos(lake_angle) * ld;
            isl.lake_z = cz + std::sin(lake_angle) * ld;
            wildIslandColumn(isl, ifloor(isl.lake_x), ifloor(isl.lake_z), c);
            isl.lake_level = std::min(c.top - 1, kIslandCeil - 6);
            const bool big_lake = isl.kind == 1 && !sat;
            isl.lake_r = big_lake ? r * (0.13f + R(10) * 0.08f) : 4.5f + R(10) * 3.5f;
            isl.lake_depth = big_lake ? 5.0f : 2.5f;
            if (!sat) {
                isl.falls = static_cast<std::uint8_t>(big_lake && R(11) < 0.55f ? 2 : 1);
                // La primera, hacia el borde mas cercano; la segunda, a un lado.
                isl.fall_angle[0] = lake_angle + (R(12) - 0.5f) * 1.0f;
                isl.fall_angle[1] = lake_angle + (R(13) < 0.5f ? 1.0f : -1.0f) * (1.4f + R(14) * 0.9f);
            }
        }
        // Ruinas: un circulo de pilares (lejos del lago).
        if (isl.kind == 3) {
            const float ra = lake ? lake_angle + 3.1415927f : R(15) * kTau;
            const float rd = R(16) * 0.18f * r;
            isl.ruin_x = cx + std::cos(ra) * rd;
            isl.ruin_z = cz + std::sin(ra) * rd;
            wildIslandColumn(isl, ifloor(isl.ruin_x), ifloor(isl.ruin_z), c);
            isl.ruin_level = std::min(c.top, kIslandCeil - 14);
            isl.ruin_r = std::min(18.0f, r * 0.22f + 5.0f);
        }
        // Cuevas.
        if (r < 22.0f) return true;
        const auto colAt = [&](float x, float z, WildIslandColumn& col) { return wildIslandColumn(isl, ifloor(x), ifloor(z), col); };
        // Lo ultimo de la isla siguiendo un rumbo desde el centro.
        const auto rimDist = [&](float a) {
            float last = 0.0f;
            WildIslandColumn col;
            for (float d = r * 0.3f; d < r * kIslandBound; d += 1.5f) {
                if (colAt(cx + std::cos(a) * d, cz + std::sin(a) * d, col)) last = d;
            }
            return last;
        };
        int nc = 0, nt = 0;
        const auto tunnel = [&](float ax, float ay, float az, float bx, float by, float bz, float rad) {
            if (nt >= 5) return;
            isl.tunnel[nt++] = WildIsland::Tunnel{ax, ay, az, bx, by, bz, rad};
        };
        // Gruta de cristal, cerca del centro.
        const float ga = R(20) * kTau;
        int grotto = -1;
        {
            const float gd = R(21) * 0.2f * r;
            const float x = cx + std::cos(ga) * gd, z = cz + std::sin(ga) * gd;
            if (colAt(x, z, c)) {
                const float rx = std::clamp(r * 0.13f, 6.0f, 15.0f);
                float ry = std::max(4.0f, rx * 0.62f);
                const float lo = static_cast<float>(c.bottom) + 6.0f, hi = static_cast<float>(c.top) - 9.0f;
                if (hi - lo > 8.0f) {
                    ry = std::min(ry, (hi - lo) * 0.5f);
                    const float y = std::clamp(static_cast<float>(c.top) - 11.0f - ry, lo + ry, hi - ry);
                    grotto = nc;
                    isl.cave[nc++] = WildIsland::Cave{x, y, z, rx, ry, rx * (0.8f + 0.4f * R(22)), 0, 0};
                }
            }
        }
        // Camara con un lago (islas grandes).
        int pool = -1;
        if (r >= 55.0f) {
            const float a = ga + 2.1f + R(23);
            const float x = cx + std::cos(a) * r * 0.28f, z = cz + std::sin(a) * r * 0.28f;
            if (colAt(x, z, c)) {
                const float rx = std::clamp(r * 0.1f, 6.0f, 11.0f), ry = 5.0f;
                const float y = static_cast<float>(c.top) - 14.0f - ry;
                if (y - ry >= static_cast<float>(c.bottom) + 10.0f) {
                    pool = nc;
                    isl.cave[nc++] = WildIsland::Cave{x, y, z, rx, ry, rx * (0.8f + 0.4f * R(24)), 1, static_cast<int>(std::floor(y)) - 1};
                }
            }
        }
        // Bocas en el acantilado: medio dentro, medio fuera (se abren al cielo).
        int mouths[2] = {-1, -1};
        const int nm = r >= 60.0f ? 2 : 1;
        for (int m = 0; m < nm; ++m) {
            const float a = ga + 1.0f + static_cast<float>(m) * 2.6f + R(25 + m) * 1.2f;
            const float rx = std::clamp(r * 0.09f, 5.0f, 9.0f), ry = rx * 0.75f;
            const float rd = rimDist(a);
            if (rd <= 0.0f) continue;
            const float x = cx + std::cos(a) * (rd - rx * 0.8f), z = cz + std::sin(a) * (rd - rx * 0.8f);
            if (!colAt(x, z, c)) continue;
            const float y = static_cast<float>(c.top) - 5.0f - ry * 0.9f;
            mouths[m] = nc;
            isl.cave[nc++] = WildIsland::Cave{x, y, z, rx, ry, rx, 2, 0};
        }
        // Tuneles entre camaras y un pozo que sale por la panza.
        const auto rad = [&](int k) { return 2.2f + R(40 + k) * 1.4f; };
        const auto& G = isl.cave[std::max(grotto, 0)];
        if (grotto >= 0) {
            for (int m = 0; m < 2; ++m) {
                if (mouths[m] < 0) continue;
                const auto& M = isl.cave[mouths[m]];
                if (m == 1 && pool >= 0) {
                    const auto& P = isl.cave[pool];
                    tunnel(P.x, static_cast<float>(P.water) + 3.5f, P.z, M.x, M.y, M.z, rad(m));
                } else {
                    tunnel(G.x, G.y, G.z, M.x, M.y, M.z, rad(m));
                }
            }
            if (pool >= 0) {
                const auto& P = isl.cave[pool];
                tunnel(G.x, G.y - G.ry * 0.3f, G.z, P.x, static_cast<float>(P.water) + 3.5f, P.z, rad(3));
            }
            const float a = ga + 3.1415927f * 0.7f;
            const float x = cx + std::cos(a) * r * 0.15f, z = cz + std::sin(a) * r * 0.15f;
            if (colAt(x, z, c)) tunnel(G.x, G.y, G.z, x, static_cast<float>(c.bottom) - 6.0f, z, rad(4) + 0.5f);
        } else if (mouths[0] >= 0 && mouths[1] >= 0) {
            const auto& A = isl.cave[mouths[0]];
            const auto& B = isl.cave[mouths[1]];
            tunnel(A.x, A.y, A.z, B.x, B.y, B.z, rad(5));
        }
        isl.caves = static_cast<std::uint8_t>(nc);
        isl.tunnels = static_cast<std::uint8_t>(nt);
        return true;
    };

    int n = 0;
    PreIsland me;
    if (pre(gx, gz, me)) {
        bool keep = true;
        for (int oz = -1; oz <= 1 && keep; ++oz) {
            for (int ox = -1; ox <= 1 && keep; ++ox) {
                if (ox == 0 && oz == 0) continue;
                PreIsland o;
                if (!pre(gx + ox, gz + oz, o)) continue;
                if (std::hypot(o.cx - me.cx, o.cz - me.cz) >= o.cluster + me.cluster + 24.0f) continue;
                if (o.prio > me.prio || (o.prio == me.prio && ox + oz * 3 > 0)) keep = false;  // gana la otra
            }
        }
        if (keep && build(out[0], me.cx, me.cz, me.r, false, 100, 0.0f)) {
            n = 1;
            for (int i = 0; i < me.sats; ++i) {
                if (build(out[n], me.sat_x[i], me.sat_z[i], me.sat_r[i], true, 200 + i * 50, out[0].top)) ++n;
            }
        }
    }
    if (owner != 0) {
        slot.owner = owner;
        slot.gx = gx;
        slot.gz = gz;
        slot.n = n;
        for (int i = 0; i < n; ++i) slot.isl[static_cast<std::size_t>(i)] = out[i];
    }
    return n;
}

void Generator::wildIslandsNear(int x0, int z0, int x1, int z1, bool /*small*/, std::vector<WildIsland>& out) const {
    out.clear();
    // Lejos del origen solamente (rapido para casi todo el mundo cercano).
    const float fx0 = static_cast<float>(x0), fz0 = static_cast<float>(z0), fx1 = static_cast<float>(x1), fz1 = static_cast<float>(z1);
    const float nx = std::clamp(0.0f, fx0, fx1), nz = std::clamp(0.0f, fz0, fz1);  // punto del rectangulo mas cerca del origen
    if (nx * nx + nz * nz < (kWildNear - 50.0f) * (kWildNear - 50.0f)) return;
    WildIsland buf[4];
    for (int gz = floorDiv(z0 - kIslandReach, kIslandCell); gz <= floorDiv(z1 + kIslandReach, kIslandCell); ++gz) {
        for (int gx = floorDiv(x0 - kIslandReach, kIslandCell); gx <= floorDiv(x1 + kIslandReach, kIslandCell); ++gx) {
            const int n = wildIslandCell(gx, gz, buf);
            for (int i = 0; i < n; ++i) {
                const WildIsland& isl = buf[i];
                const float m = isl.r * kIslandBound + 1.0f;
                if (isl.cx + m < fx0 || isl.cx - m > fx1 || isl.cz + m < fz0 || isl.cz - m > fz1) continue;
                out.push_back(isl);
            }
        }
    }
}

// --- Lo suelto de un chunk ----------------------------------------------------------------

void Generator::placeWild(ChunkData& chunk, const std::array<int, kChunkSize * kChunkSize>& heights) const {
    const int x0 = chunk.cx * kChunkSize, z0 = chunk.cz * kChunkSize;
    {
        // Nada lejos del origen: el mundo de siempre.
        const float nx = std::clamp(0.0f, static_cast<float>(x0 - 40), static_cast<float>(x0 + kChunkSize + 40));
        const float nz = std::clamp(0.0f, static_cast<float>(z0 - 40), static_cast<float>(z0 + kChunkSize + 40));
        if (nx * nx + nz * nz < (kWildNear - 100.0f) * (kWildNear - 100.0f)) return;
    }
    Put w{chunk, x0, z0};
    const auto seed = static_cast<std::uint32_t>(seed_) + 9307u;
    const auto ground = [&](int wx, int wz) {
        const int lx = wx - x0, lz = wz - z0;
        if (lx >= 0 && lz >= 0 && lx < kChunkSize && lz < kChunkSize) return heights[static_cast<std::size_t>(lx + lz * kChunkSize)];
        return adventureColumn(wx, wz).height;
    };

    // --- Agujas, cristales y respiraderos (celdas de 36 m, de los vecinos tambien) ---
    {
        constexpr int kCell = 36, kReach = 22;
        for (int gz = floorDiv(z0 - kReach, kCell); gz <= floorDiv(z0 + kChunkSize + kReach, kCell); ++gz) {
            for (int gx = floorDiv(x0 - kReach, kCell); gx <= floorDiv(x0 + kChunkSize + kReach, kCell); ++gx) {
                const float roll = rand01(gx, 0, gz, seed);
                if (roll > 0.5f) continue;
                const int tx = gx * kCell + 4 + static_cast<int>(rand01(gx, 1, gz, seed) * (kCell - 8));
                const int tz = gz * kCell + 4 + static_cast<int>(rand01(gx, 2, gz, seed) * (kCell - 8));
                const WildCell region = wildRegion(static_cast<float>(tx), static_cast<float>(tz));
                if (region.region < WBadlands || region.strength < 0.3f) continue;
                const AdventureColumn c = adventureColumn(tx, tz);
                if (c.water > c.height || c.lava > c.height || c.river > 0.1f || c.feature != 0) continue;
                const int base = c.height;
                if (c.biome == AB::ARoseDesert && roll < 0.5f) {
                    // Racimo de agujas de cuarzo rosa con la punta de cristal.
                    const int n = 1 + static_cast<int>(rand01(gx, 3, gz, seed) * 3.0f);
                    for (int i = 0; i < n; ++i) {
                        const float dx = (rand01(gx, 10 + i, gz, seed) - 0.5f) * 1.1f, dz = (rand01(gx, 20 + i, gz, seed) - 0.5f) * 1.1f;
                        const float len = std::sqrt(dx * dx + dz * dz + 1.0f);
                        const float size = rand01(gx, 30 + i, gz, seed);
                        spire(w, static_cast<float>(tx) + 0.5f, static_cast<float>(base) - 2.0f, static_cast<float>(tz) + 0.5f, dx / len,
                              1.0f / len, dz / len, 8.0f + size * 22.0f, 1.2f + size * 2.0f, block::RoseQuartz, block::CrystalPink, 0.18f);
                    }
                } else if (c.biome == AB::ABlight && roll < 0.12f) {
                    // Racimo de cristal violeta.
                    const int n = 2 + static_cast<int>(rand01(gx, 3, gz, seed) * 3.0f);
                    for (int i = 0; i < n; ++i) {
                        const float dx = (rand01(gx, 10 + i, gz, seed) - 0.5f) * 1.2f, dz = (rand01(gx, 20 + i, gz, seed) - 0.5f) * 1.2f;
                        const float len = std::sqrt(dx * dx + dz * dz + 1.0f);
                        spire(w, static_cast<float>(tx) + 0.5f, static_cast<float>(base) - 1.0f, static_cast<float>(tz) + 0.5f, dx / len,
                              1.0f / len, dz / len, 3.0f + rand01(gx, 30 + i, gz, seed) * 5.0f, 0.9f, block::CrystalViolet,
                              block::CrystalViolet, 0.0f);
                    }
                } else if (c.biome == AB::AAshlands && roll < 0.3f) {
                    // Respiradero: magma en el centro y una costra de azufre alrededor.
                    const float radius = 2.0f + rand01(gx, 3, gz, seed) * 2.5f;
                    const int ri = static_cast<int>(std::ceil(radius));
                    for (int dz = -ri; dz <= ri; ++dz) {
                        for (int dx = -ri; dx <= ri; ++dx) {
                            if (!w.inside(tx + dx, 1, tz + dz)) continue;
                            const float d = std::hypot(static_cast<float>(dx), static_cast<float>(dz));
                            if (d > radius) continue;
                            const int y = ground(tx + dx, tz + dz);
                            const BlockId top = w.get(tx + dx, y, tz + dz);
                            if (top != block::Ash && top != block::Basalt) continue;
                            w.put(tx + dx, y, tz + dz, d < radius * 0.4f ? block::Magma : block::Brimstone, false);
                        }
                    }
                }
            }
        }
    }

    // --- Islas flotantes ---
    std::vector<WildIsland> islands;
    wildIslandsNear(x0 - 8, z0 - 8, x0 + kChunkSize + 7, z0 + kChunkSize + 7, true, islands);
    if (islands.empty()) return;
    const auto s_isl = seed + 50u;
    const float fx0 = static_cast<float>(x0), fz0 = static_cast<float>(z0);
    const float fx1 = fx0 + static_cast<float>(kChunkSize), fz1 = fz0 + static_cast<float>(kChunkSize);
    const auto at = [&](int lx, int y, int lz) -> BlockId& { return chunk.blocks[static_cast<std::size_t>(cellIndex(lx, y, lz))]; };
    const auto crystalOf = [](const WildIsland& isl, float r) {
        if (r < 0.25f) return block::CrystalPink;
        switch (isl.id % 3u) {
            case 0: return block::SkyCrystal;
            case 1: return block::CrystalBlue;
            default: return block::CrystalViolet;
        }
    };

    // Camaras y tuneles que tocan este chunk.
    struct Feature {
        const WildIsland* isl = nullptr;
        bool tunnel = false;
        int index = 0;
        float minx = 0, maxx = 0, minz = 0, maxz = 0, miny = 0, maxy = 0;
    };
    std::vector<Feature> feats;
    for (const WildIsland& isl : islands) {
        for (int i = 0; i < isl.caves; ++i) {
            const WildIsland::Cave& c = isl.cave[i];
            const float mx = c.rx * 1.3f + 1.0f, mz = c.rz * 1.3f + 1.0f, my = c.ry * 1.3f + 1.0f;
            Feature f{&isl, false, i, c.x - mx, c.x + mx, c.z - mz, c.z + mz, c.y - my, c.y + my};
            if (f.maxx < fx0 || f.minx > fx1 || f.maxz < fz0 || f.minz > fz1) continue;
            feats.push_back(f);
        }
        for (int i = 0; i < isl.tunnels; ++i) {
            const WildIsland::Tunnel& tn = isl.tunnel[i];
            const float m = tn.r * 1.4f + 4.0f;
            Feature f{&isl, true, i, std::min(tn.ax, tn.bx) - m, std::max(tn.ax, tn.bx) + m, std::min(tn.az, tn.bz) - m,
                      std::max(tn.az, tn.bz) + m, std::min(tn.ay, tn.by) - m, std::max(tn.ay, tn.by) + m};
            if (f.maxx < fx0 || f.minx > fx1 || f.maxz < fz0 || f.minz > fz1) continue;
            feats.push_back(f);
        }
    }
    // 0 nada; 1 gruta; 2 camara del lago (agua hasta `water`); 3 boca; 4 tunel.
    const auto caveAt = [&](const Feature* const* list, std::size_t n, int x, int y, int z, int& water) {
        const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f, pz = static_cast<float>(z) + 0.5f;
        float rough = 9.0f;
        const auto noise = [&]() {
            if (rough > 8.0f) rough = noise3(px / 6.0f, py / 6.0f, pz / 6.0f, 870);
            return rough;
        };
        for (std::size_t i = 0; i < n; ++i) {
            const Feature& f = *list[i];
            if (py < f.miny || py > f.maxy) continue;
            if (!f.tunnel) {
                const WildIsland::Cave& c = f.isl->cave[f.index];
                const float dx = (px - c.x) / c.rx, dz = (pz - c.z) / c.rz;
                float dy = (py - c.y) / c.ry;
                if (dy < 0.0f) dy *= 1.35f;  // el suelo, mas plano
                const float q = dx * dx + dy * dy + dz * dz;
                if (q > 1.7f) continue;
                if (q < 1.0f + noise() * 0.7f) {
                    water = c.water;
                    return c.kind == 0 ? 1 : (c.kind == 1 ? 2 : 3);
                }
            } else {
                const WildIsland::Tunnel& tn = f.isl->tunnel[f.index];
                const float abx = tn.bx - tn.ax, aby = tn.by - tn.ay, abz = tn.bz - tn.az;
                const float len2 = std::max(1e-3f, abx * abx + aby * aby + abz * abz);
                const float u = std::clamp(((px - tn.ax) * abx + (py - tn.ay) * aby + (pz - tn.az) * abz) / len2, 0.0f, 1.0f);
                {  // lejos de la recta del tunel (con todo su meneo): ni mirarlo
                    const float sx = px - tn.ax - abx * u, sy = py - tn.ay - aby * u, sz = pz - tn.az - abz * u;
                    const float reach = tn.r * 1.55f + 3.0f;
                    if (sx * sx + sy * sy + sz * sz > reach * reach) continue;
                }
                const float hl = std::sqrt(abx * abx + abz * abz);
                const float wob = std::sin(u * 7.0f + tn.r * 13.0f) * 2.5f * std::min(1.0f, u * 4.0f) * std::min(1.0f, (1.0f - u) * 4.0f);
                const float qx = tn.ax + abx * u - (hl > 1e-3f ? abz / hl : 0.0f) * wob;
                const float qz = tn.az + abz * u + (hl > 1e-3f ? abx / hl : 0.0f) * wob;
                const float qy = tn.ay + aby * u + std::sin(u * 11.0f + tn.r * 7.0f) * 1.5f * std::min(1.0f, u * 4.0f) * std::min(1.0f, (1.0f - u) * 4.0f);
                const float rad = tn.r * (0.85f + 0.3f * std::sin(u * 13.0f + tn.r * 5.0f));
                const float d2 = (px - qx) * (px - qx) + (py - qy) * (py - qy) + (pz - qz) * (pz - qz);
                if (d2 > rad * rad * 1.8f) continue;
                if (d2 < rad * rad * (1.0f + noise() * 0.8f)) return 4;
            }
        }
        return 0;
    };

    std::array<std::uint8_t, kWorldHeight> carved{};
    std::vector<const Feature*> here;
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            const int wx = x0 + x, wz = z0 + z;
            const std::size_t ci = static_cast<std::size_t>(x + z * kChunkSize);
            const int floor_y = std::max(heights[ci], sea_) + 10;  // nunca pegada al suelo (ni a sus arboles)
            int highest = -1;
            const WildIsland* hisl = nullptr;
            WildIslandColumn hcol;
            for (const WildIsland& isl : islands) {
                WildIslandColumn c;
                if (!wildIslandColumn(isl, wx, wz, c)) continue;
                const int bottom = std::max(c.bottom, floor_y);
                if (bottom > c.top) continue;
                here.clear();
                const float px = static_cast<float>(wx) + 0.5f, pz = static_cast<float>(wz) + 0.5f;
                for (const Feature& f : feats) {
                    if (f.isl == &isl && px >= f.minx && px <= f.maxx && pz >= f.minz && pz <= f.maxz) here.push_back(&f);
                }
                const bool lake = c.water > c.top;
                const bool paved = isl.ruin_r > 0.0f && std::hypot(px - isl.ruin_x, pz - isl.ruin_z) < isl.ruin_r;
                const int soil = c.rock ? 0 : 3 + (rand01(wx, 1, wz, s_isl) < 0.5f ? 1 : 0);
                const int stone_line = c.bottom + (c.top - c.bottom) * 45 / 100;
                for (int y = bottom; y <= c.top; ++y) {
                    const int depth = c.top - y;
                    BlockId id;
                    if (depth == 0) {
                        if (lake) {
                            id = rand01(wx, 2, wz, s_isl) < 0.6f ? block::Sand : block::Clay;
                        } else if (c.rock) {
                            id = c.top >= 226 ? block::Snow : (rand01(wx, 2, wz, s_isl) < 0.15f ? block::Gravel : block::Stone);
                        } else if (paved) {
                            const float r = rand01(wx, 2, wz, s_isl);
                            id = r < 0.35f   ? block::StoneBricks
                                 : r < 0.6f  ? block::MossyBricks
                                 : r < 0.75f ? block::CrackedBricks
                                 : r < 0.85f ? block::Cobblestone
                                             : block::Grass;
                        } else {
                            id = block::Grass;
                        }
                    } else if (depth <= soil && y > bottom + 1) {
                        id = lake ? block::Sand : block::Dirt;
                    } else {
                        id = y < stone_line ? block::SkyStone : block::Stone;
                        // Vetas de mena (bolsas de 4 x 4 x 4 a medio llenar).
                        if (rand01(wx >> 2, y >> 2, wz >> 2, s_isl + 1u) < 0.07f && rand01(wx, y, wz, s_isl + 3u) < 0.55f) {
                            const float k = rand01(wx >> 2, (y >> 2) + 100, wz >> 2, s_isl + 1u);
                            id = k < 0.38f   ? block::CoalOre
                                 : k < 0.62f ? block::IronOre
                                 : k < 0.74f ? block::CopperOre
                                 : k < 0.84f ? block::GoldOre
                                 : k < 0.97f ? block::SkyCrystal
                                             : block::DiamondOre;
                        }
                    }
                    carved[static_cast<std::size_t>(y)] = 0;
                    if (!here.empty() && y <= c.top - 5) {
                        int water = 0;
                        int k = caveAt(here.data(), here.size(), wx, y, wz, water);
                        if (k == 2 && y <= water && y < c.bottom + 3) k = 0;  // el agua, siempre sobre roca
                        if (k != 0) {
                            carved[static_cast<std::size_t>(y)] = static_cast<std::uint8_t>(k);
                            id = k == 2 && y <= water ? block::Water : block::Air;
                        }
                    }
                    at(x, y, z) = id;
                }
                for (int y = c.top + 1; y <= c.water; ++y) at(x, y, z) = block::Water;
                // Las cuevas por dentro: cristales y enredaderas del techo, musgo y setas del suelo.
                if (!here.empty()) {
                    const BlockId crystal = crystalOf(isl, rand01(wx, 4, wz, s_isl));
                    for (int y = std::min(c.top - 5, kWorldHeight - 2); y > bottom; --y) {
                        const std::uint8_t k = carved[static_cast<std::size_t>(y)];
                        if (k == 0 || at(x, y, z) != block::Air) continue;
                        const float r = rand01(wx, y, wz, s_isl + 5u);
                        if (carved[static_cast<std::size_t>(y + 1)] == 0 && y + 1 <= c.top) {  // techo
                            int len = 0;
                            BlockId hang = block::Air;
                            if (k == 1 && r < 0.16f) {
                                hang = crystal;
                                len = 1 + static_cast<int>(rand01(wx, y, wz, s_isl + 6u) * 3.0f);
                            } else if (r < 0.04f) {
                                hang = block::GlowVines;
                                len = 1 + static_cast<int>(rand01(wx, y, wz, s_isl + 6u) * 4.0f);
                            } else if (r < 0.08f) {
                                hang = block::Stalactite;
                                len = 1;
                            }
                            for (int i = 0; i < len && y - i > bottom && carved[static_cast<std::size_t>(y - i)] != 0 &&
                                            at(x, y - i, z) == block::Air;
                                 ++i)
                                at(x, y - i, z) = hang;
                            continue;
                        }
                        if (carved[static_cast<std::size_t>(y - 1)] == 0 && y - 1 >= bottom) {  // suelo
                            if (k == 1) {
                                if (r < 0.5f) at(x, y - 1, z) = block::Moss;
                                if (r > 0.9f) {
                                    const int len = 1 + static_cast<int>(rand01(wx, y, wz, s_isl + 6u) * 3.0f);
                                    for (int i = 0; i < len && y + i <= c.top - 5 && at(x, y + i, z) == block::Air; ++i) at(x, y + i, z) = crystal;
                                } else if (r > 0.84f) {
                                    at(x, y, z) = block::GlowMushroom;
                                }
                            } else if (k == 3) {
                                if (r < 0.6f) at(x, y - 1, z) = block::Moss;
                                if (r < 0.15f) at(x, y, z) = block::TallGrass;
                            } else if (k == 4) {
                                if (r < 0.12f) at(x, y - 1, z) = block::Moss;
                                else if (r > 0.985f) at(x, y, z) = block::GlowMushroom;
                            }
                        }
                    }
                }
                // Lo que cuelga de la panza: cristales, enredaderas y raices.
                if (bottom == c.bottom && bottom - 2 > floor_y && blockDef(at(x, bottom, z)).solid) {
                    const float r = rand01(wx, 3, wz, s_isl + 2u);
                    const float r2 = rand01(wx, 4, wz, s_isl + 2u);
                    int len = 0;
                    BlockId hang = block::Air;
                    if (c.t < 0.75f && r < 0.006f) {
                        hang = crystalOf(isl, r2);
                        len = 1 + static_cast<int>(r2 * 4.0f);
                    } else if (r < 0.009f && c.t > 0.3f) {
                        hang = block::GlowVines;
                        len = 2 + static_cast<int>(r2 * 6.0f);
                    } else if (r < 0.015f && c.t > 0.55f && isl.kind != 2) {
                        hang = block::OakLog;  // raices
                        len = 2 + static_cast<int>(r2 * 4.0f);
                    }
                    for (int i = 1; i <= len && bottom - i > floor_y; ++i) {
                        if (at(x, bottom - i, z) != block::Air) break;
                        at(x, bottom - i, z) = hang;
                    }
                }
                if (c.top > highest) {
                    highest = c.top;
                    hisl = &isl;
                    hcol = c;
                }
            }
            if (highest < 0) continue;
            // Cesped verde vivo (el tinte es de la columna: tambien el suelo de debajo).
            chunk.grass_tint[ci] = packTint(0.70f, 1.0f, 0.62f);
            chunk.foliage_tint[ci] = packTint(0.66f, 1.0f, 0.56f);
            // Hierba y flores segun el tipo de isla.
            if (highest + 1 < kWorldHeight && hcol.water == 0 && at(x, highest, z) == block::Grass && at(x, highest + 1, z) == block::Air) {
                const float r = rand01(wx, 7, wz, s_isl);
                const float r2 = rand01(wx, 8, wz, s_isl);
                BlockId plant = block::Air;
                switch (hisl->kind) {
                    case 0:  // bosque
                        plant = r < 0.30f ? block::TallGrass : r < 0.36f ? block::Fern : r < 0.39f ? block::Bush
                                : r < 0.41f ? (r2 < 0.5f ? block::RedMushroom : block::BrownMushroom) : block::Air;
                        break;
                    case 1: {  // pradera: muchas flores
                        static constexpr BlockId kFlowers[6] = {block::SkyFlower, block::WhiteFlower, block::BlueFlower,
                                                                block::RedFlower, block::YellowFlower, block::Lavender};
                        plant = r < 0.32f ? block::TallGrass : r < 0.42f ? kFlowers[static_cast<int>(r2 * 5.99f)] : block::Air;
                        break;
                    }
                    case 2: plant = r < 0.15f ? block::TallGrass : r < 0.17f ? block::BlueFlower : block::Air; break;
                    default: plant = r < 0.25f ? block::TallGrass : r < 0.29f ? (r2 < 0.5f ? block::WhiteFlower : block::SkyFlower) : block::Air; break;
                }
                if (plant != block::Air) at(x, highest + 1, z) = plant;
            }
        }
    }

    // --- Ruinas: un circulo de pilares rotos, dinteles y un monolito ---
    for (const WildIsland& isl : islands) {
        if (isl.ruin_r <= 0.0f) continue;
        const float m = isl.ruin_r + 3.0f;
        if (isl.ruin_x + m < fx0 || isl.ruin_x - m > fx1 || isl.ruin_z + m < fz0 || isl.ruin_z - m > fz1) continue;
        const std::uint32_t s = s_isl + 90u + isl.id;
        const auto stoneMix = [&](int x, int y, int z) {
            const float r = rand01(x, y, z, s);
            return r < 0.45f ? block::StoneBricks : (r < 0.75f ? block::MossyBricks : block::CrackedBricks);
        };
        const auto topAt = [&](int x, int z) {
            WildIslandColumn c;
            return wildIslandColumn(isl, x, z, c) ? c.top : -1;
        };
        constexpr int kPillars = 11;
        const float rr = isl.ruin_r - 3.0f;
        int px[kPillars], pz[kPillars], ph[kPillars], pb[kPillars];
        for (int i = 0; i < kPillars; ++i) {
            const float a = static_cast<float>(i) * 6.2831853f / kPillars + rand01(i, 1, 0, s) * 0.2f;
            px[i] = ifloor(isl.ruin_x + std::cos(a) * rr);
            pz[i] = ifloor(isl.ruin_z + std::sin(a) * rr);
            const float hr = rand01(i, 2, 0, s);
            ph[i] = hr < 0.3f ? 1 + static_cast<int>(hr * 6.0f) : 5 + static_cast<int>(rand01(i, 3, 0, s) * 5.0f);
            int base = -1;
            for (int k = 0; k < 4; ++k) base = std::max(base, topAt(px[i] + (k & 1), pz[i] + (k >> 1)));
            pb[i] = base;
            if (base < 0) continue;
            for (int k = 0; k < 4; ++k) {
                const int qx = px[i] + (k & 1), qz = pz[i] + (k >> 1);
                const int from = topAt(qx, qz);
                if (from < 0) continue;
                // Los pilares rotos: alguna esquina mas baja.
                const int h = ph[i] - (rand01(qx, 4, qz, s) < 0.3f ? 1 + static_cast<int>(rand01(qx, 5, qz, s) * 2.0f) : 0);
                for (int y = from; y <= base + h && y < kWorldHeight; ++y) w.put(qx, y, qz, y == from ? block::StoneBricks : stoneMix(qx, y, qz), false);
            }
        }
        // Dinteles entre pilares altos.
        for (int i = 0; i < kPillars; ++i) {
            const int j = (i + 1) % kPillars;
            if (pb[i] < 0 || pb[j] < 0 || ph[i] < 6 || ph[j] < 6 || rand01(i, 6, 0, s) > 0.6f) continue;
            const int y = std::min(pb[i] + ph[i], pb[j] + ph[j]);
            const float ax = static_cast<float>(px[i]) + 1.0f, az = static_cast<float>(pz[i]) + 1.0f;
            const float bx = static_cast<float>(px[j]) + 1.0f, bz = static_cast<float>(pz[j]) + 1.0f;
            for (float u = 0.0f; u <= 1.0f; u += 0.05f) {
                const int qx = ifloor(ax + (bx - ax) * u), qz = ifloor(az + (bz - az) * u);
                w.put(qx, y, qz, stoneMix(qx, y, qz), true);
            }
        }
        // Monolito de runas en el centro.
        const int cx = ifloor(isl.ruin_x), cz = ifloor(isl.ruin_z);
        const int ct = topAt(cx, cz);
        if (ct > 0) {
            for (int y = ct + 1; y <= ct + 4 && y < kWorldHeight; ++y) w.put(cx, y, cz, block::RuneStone, false);
            w.put(cx + 1, ct + 1, cz, block::Candles, true);
        }
    }

    // --- Cascadas: del lago (o manantial) al borde por un cauce, y por el borde hasta abajo ---
    const auto fallCorridor = [](const WildIsland& isl, int f, float& ux, float& uz, float& len) {
        ux = std::cos(isl.fall_angle[f]);
        uz = std::sin(isl.fall_angle[f]);
        len = isl.r * kIslandBound * 2.0f + 4.0f;
    };
    for (const WildIsland& isl : islands) {
        for (int f = 0; f < isl.falls && isl.lake_r > 0.0f; ++f) {
            float ux = 0.0f, uz = 0.0f, len = 0.0f;
            fallCorridor(isl, f, ux, uz, len);
            // Solo si el cauce (o la cascada) pasa por este chunk.
            {
                const float m = 6.0f;
                const float minx = std::min(isl.lake_x, isl.lake_x + ux * len) - m, maxx = std::max(isl.lake_x, isl.lake_x + ux * len) + m;
                const float minz = std::min(isl.lake_z, isl.lake_z + uz * len) - m, maxz = std::max(isl.lake_z, isl.lake_z + uz * len) + m;
                if (maxx < fx0 || minx > fx1 || maxz < fz0 || minz > fz1) continue;
                // Distancia del chunk a la recta del cauce.
                const float ccx = fx0 + 8.0f - isl.lake_x, ccz = fz0 + 8.0f - isl.lake_z;
                if (std::abs(ccx * -uz + ccz * ux) > 12.0f + 6.0f) continue;
            }
            const int half = isl.lake_r > 10.0f ? 2 : 1;
            for (int lane = -half; lane <= half; ++lane) {
                const float off = static_cast<float>(lane);
                int level = isl.lake_level;
                for (float d = 0.0f; d < len; d += 0.7f) {
                    const float qx = isl.lake_x + ux * d - uz * off, qz = isl.lake_z + uz * d + ux * off;
                    const int ix = ifloor(qx), iz = ifloor(qz);
                    WildIslandColumn c;
                    if (!wildIslandColumn(isl, ix, iz, c)) {
                        // El borde: la cortina de agua (2 de grueso) hasta lo primero que encuentre.
                        for (int k = 0; k < 2; ++k) {
                            const int fx = ifloor(qx + ux * static_cast<float>(k)), fz = ifloor(qz + uz * static_cast<float>(k));
                            if (!w.inside(fx, 1, fz)) continue;
                            for (int y = level; y > 1; --y) {
                                const BlockId cell = w.get(fx, y, fz);
                                if (cell != block::Air && cell != block::FallingWater && !blockDef(cell).replaceable) break;
                                w.put(fx, y, fz, block::FallingWater, false);
                            }
                        }
                        break;
                    }
                    if (c.water > c.top) {
                        level = c.water;
                        continue;
                    }
                    level = std::min(level, c.top - 1);
                    if (!w.inside(ix, 1, iz)) continue;
                    w.put(ix, level, iz, block::FallingWater, false);
                    for (int y = level + 1; y <= c.top + 1 && y < kWorldHeight; ++y) w.put(ix, y, iz, block::Air, false);
                }
            }
        }
    }

    // --- Arboles de las islas: bosques espesos, alguno suelto en las praderas, abetos en los picos ---
    {
        constexpr int kCell = 5, kReach = 7;
        const std::uint32_t s = s_isl + 70u;
        const auto nearFall = [&](const WildIsland& isl, float x, float z) {
            for (int f = 0; f < isl.falls && isl.lake_r > 0.0f; ++f) {
                float ux = 0.0f, uz = 0.0f, len = 0.0f;
                fallCorridor(isl, f, ux, uz, len);
                const float dx = x - isl.lake_x, dz = z - isl.lake_z;
                const float along = dx * ux + dz * uz;
                if (along > -2.0f && along < len && std::abs(dx * -uz + dz * ux) < 5.0f) return true;
            }
            return false;
        };
        for (int gz = floorDiv(z0 - kReach, kCell); gz <= floorDiv(z0 + kChunkSize + kReach, kCell); ++gz) {
            for (int gx = floorDiv(x0 - kReach, kCell); gx <= floorDiv(x0 + kChunkSize + kReach, kCell); ++gx) {
                const float roll = rand01(gx, 0, gz, s);
                if (roll > 0.6f) continue;
                const int tx = gx * kCell + static_cast<int>(rand01(gx, 1, gz, s) * kCell);
                const int tz = gz * kCell + static_cast<int>(rand01(gx, 2, gz, s) * kCell);
                const WildIsland* on = nullptr;
                WildIslandColumn best;
                for (const WildIsland& isl : islands) {
                    WildIslandColumn c;
                    if (!wildIslandColumn(isl, tx, tz, c)) continue;
                    if (on == nullptr || c.top > best.top) {
                        on = &isl;
                        best = c;
                    }
                }
                if (on == nullptr || best.water > 0 || best.rock || best.t > 0.9f || best.top + 18 >= kWorldHeight) continue;
                static constexpr float kDensity[4] = {0.6f, 0.05f, 0.06f, 0.06f};
                if (roll > kDensity[on->kind & 3]) continue;
                const float fx = static_cast<float>(tx) + 0.5f, fz = static_cast<float>(tz) + 0.5f;
                if (on->ruin_r > 0.0f && std::hypot(fx - on->ruin_x, fz - on->ruin_z) < on->ruin_r + 2.0f) continue;
                if (nearFall(*on, fx, fz)) continue;
                if (std::max(best.bottom, std::max(ground(tx, tz), sea_) + 10) > best.top - 4) continue;
                const int base = best.top + 1;
                const float size = rand01(gx, 4, gz, s);
                const float kind = rand01(gx, 5, gz, s);
                if (on->kind == 2) {  // abeto
                    const int h = 7 + static_cast<int>(size * 5.0f);
                    for (int i = 0; i < h; ++i) w.put(tx, base + i, tz, block::SpruceLog, false);
                    for (int i = 2; i <= h; ++i) {
                        const float rad = (1.0f - static_cast<float>(i - 2) / static_cast<float>(h - 1)) * (2.2f + size * 1.2f) + 0.4f;
                        const int ri = static_cast<int>(std::ceil(rad));
                        for (int dz = -ri; dz <= ri; ++dz)
                            for (int dx = -ri; dx <= ri; ++dx)
                                if (static_cast<float>(dx * dx + dz * dz) <= rad * rad && (dx != 0 || dz != 0 || i == h))
                                    w.put(tx + dx, base + i, tz + dz, block::SpruceLeaves, true);
                    }
                    w.put(tx, base + h, tz, block::SpruceLeaves, true);
                } else {
                    const bool golden = on->golden && kind < 0.6f;
                    const bool birch = !golden && on->kind == 0 && kind > 0.72f;
                    const bool large = !birch && size > 0.82f;
                    const int h = large ? 8 + static_cast<int>(size * 4.0f) : 4 + static_cast<int>(size * 4.0f);
                    const float radius = large ? 4.0f + size : 2.3f + size * 1.4f;
                    const BlockId log = golden ? block::GoldenLog : (birch ? block::BirchLog : block::OakLog);
                    const BlockId leaves = golden ? block::GoldenLeaves : (birch ? block::BirchLeaves : block::OakLeaves);
                    canopy(w, tx, base + h, tz, radius, radius * 0.75f, leaves, s + 9u);
                    if (large) canopy(w, tx + 2, base + h - 2, tz - 1, radius * 0.7f, radius * 0.55f, leaves, s + 10u);
                    for (int i = 0; i < h; ++i) w.put(tx, base + i, tz, log, false);
                }
                w.put(tx, base - 1, tz, block::Dirt, false);
            }
        }
    }
}

}  // namespace cramion::voxel
