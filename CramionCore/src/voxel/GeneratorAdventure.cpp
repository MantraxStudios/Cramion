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
//     suelo con musgo, setas y setas luminosas (dan luz). Entradas que se
//     recorren andando (bocas, simas con rampa y grietas): GeneratorCaves.cpp.
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
        bool ok = center.height > sea_ + 3 && center.river < 0.05f &&
                  (center.region < WBadlands || center.region == WBlight || center.strength <= 0.0f ||
                   (isLushRegion(center.region) && center.region != WSwamp));  // NOCHE-MUNDO: lagos tambien en los frondosos
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
        // OCEANOS-HZ: fuera de las regiones del origen, menos lagos sueltos (uno de cada tres). Solo si
        // el lago entero (radio + orilla < 30 m) queda fuera: los de dentro no cambian.
        if (lake.valid && std::max(std::abs(lake.x), std::abs(lake.z)) >= static_cast<float>(oceanEdge() + 32) &&
            oceanScaleWeight(lake.x, lake.z) > 0.0f && rand01(gx, 16, gz, s) > 0.35f) {
            lake.valid = false;
        }
    }
    if (lakes_) {
        std::lock_guard lock(lakes_->mutex);
        if (lakes_->cells.size() > 50000) lakes_->cells.clear();
        lakes_->cells[key] = lake;
    }
    last = Last{cache_id, key, lake};
    return lake;
}

// --- Oceanos hondos ----------------------------------------------------------------------
// Lejos del pueblo de inicio (a partir de ~420 m; del todo a ~760 m) el fondo del mar
// deja de ser el de siempre (como mucho ~30 m bajo el agua) y pasa a tener plataforma
// continental junto a la costa, talud con canones, llanura abisal a y ~12-18 con colinas,
// dorsales de basalto, montes submarinos (alguno asoma como isla) y fosas que bajan casi
// hasta la roca madre. Y a partir de ~1 km el continente se hunde en zonas de ~3 km:
// oceanos grandes (el mundo de cerca casi no tiene mar). Todo funcion pura de (semilla, x, z).
namespace {
constexpr float kOceanNear = 420.0f, kOceanFull = 760.0f;    // el fondo hondo
constexpr float kBasinNear = 1050.0f, kBasinFull = 1800.0f;  // los oceanos grandes
constexpr int kSeamountCell = 240;
}  // namespace

float Generator::oceanBasin(float x, float z) const {
    if (!deep_oceans_) return 0.0f;
    const float d2 = x * x + z * z;
    if (d2 <= kBasinNear * kBasinNear) return 0.0f;
    const float n = fbm2(x / 3400.0f, z / 3400.0f, 3, 401);
    const float basin = smoothstep(-0.06f, 0.30f, n);
    if (basin <= 0.0f) return 0.0f;
    float g = smoothstep(kBasinNear, kBasinFull, std::sqrt(d2));
    // Los desiertos calidos y secos se quedan en tierra (interior de los continentes).
    const float hot = smoothstep(0.1f, 0.4f, fbm2(x / 1200.0f, z / 1200.0f, 3, 107) * 1.7f);
    if (hot > 0.0f) g *= 1.0f - hot * (1.0f - smoothstep(-0.2f, 0.15f, fbm2(x / 1000.0f, z / 1000.0f, 3, 108) * 1.7f));
    return g * 0.40f * basin;
}

float Generator::oceanWeight(float x, float z) const {
    if (!deep_oceans_) return 0.0f;
    const float d2 = x * x + z * z;
    if (d2 <= kOceanNear * kOceanNear) return 0.0f;
    return smoothstep(kOceanNear, kOceanFull, std::sqrt(d2));
}

float Generator::oceanFloor(float x, float z, float e, float hills, int& seabed) const {
    const float sea = static_cast<float>(sea_);
    seabed = 0;
    // Perfil por la distancia a la costa (e = -continente): plataforma, talud y llanura.
    const float shelf = smoothstep(0.0f, 0.045f, e);
    const float slope = smoothstep(0.035f, 0.12f, e);
    const float abyss = smoothstep(0.10f, 0.22f, e);
    float depth = 2.0f + 12.0f * shelf + 30.0f * slope + 4.0f * abyss;  // 2 .. 48 bajo el mar
    // OCEANOS-HZ: en los mares grandes, la llanura a ~50 bajo el agua con cuencas y lomas de ~1 km.
    const float ow = oceanScaleWeight(x, z);
    if (ow > 0.0f) depth += ow * (2.0f * slope + fbm2(x / 1400.0f, z / 1400.0f, 3, 407) * 9.0f * abyss);
    // Colinas abisales y detalle (la llanura no es una mesa).
    depth += hills * (2.0f + 4.0f * slope);
    depth += fbm2(x / 46.0f, z / 46.0f, 2, 402) * (1.0f + 2.5f * abyss);
    // Canones submarinos que bajan por el talud.
    const float canyon = std::abs(fbm2(x / 320.0f, z / 320.0f, 3, 403));
    if (ow <= 0.0f) depth += (1.0f - smoothstep(0.012f, 0.05f, canyon)) * 14.0f * slope * (1.0f - 0.6f * abyss);
    else depth += (1.0f - smoothstep(0.012f, 0.05f, canyon)) * 14.0f * slope * (1.0f - (0.6f + 0.4f * ow) * abyss);  // OCEANOS-HZ: solo en el talud
    // Dorsal: cordilleras submarinas largas.
    const float rl = 1.0f - std::abs(fbm2(x / 900.0f, z / 900.0f, 3, 404));
    float r8 = std::pow(rl, 8.0f);
    // OCEANOS-HZ: dorsales mas finas y solo en algunas zonas de ~3 km (si no, una red por todo el fondo).
    if (ow > 0.0f) r8 = lerp(r8, r8 * r8 * smoothstep(0.0f, 0.25f, fbm2(x / 3000.0f, z / 3000.0f, 2, 409)), ow);
    const float ridge = r8 * 26.0f * abyss;
    depth -= ridge;
    if (ridge > 9.0f) seabed = 1;
    // Fosas: estrechas y muy hondas, en mitad de la llanura.
    const float tr = std::abs(fbm2(x / 1500.0f + 7.3f, z / 1500.0f, 3, 405));
    float trench = (1.0f - smoothstep(0.010f, 0.042f, tr)) * smoothstep(0.16f, 0.28f, e);
    // OCEANOS-HZ: en los mares grandes las fosas son raras (solo en algunas zonas de ~4 km).
    if (ow > 0.0f && trench > 0.0f) trench *= lerp(1.0f, smoothstep(0.12f, 0.30f, fbm2(x / 4000.0f, z / 4000.0f, 2, 408)), ow);
    depth += trench * 48.0f;
    if (trench > 0.35f) seabed = 2;
    if (ow > 0.0f) depth = lerp(depth, std::max(depth, 1.5f), ow);  // OCEANOS-HZ: nada asoma junto a la costa (salvo los montes)
    float y = sea - depth;
    // Montes submarinos: conos redondeados (uno como mucho por celda de 240 m).
    if (abyss > 0.0f) {
        const auto s = static_cast<std::uint32_t>(seed_) + 6007u;
        const int gx = floorDivInt(static_cast<int>(std::floor(x)), kSeamountCell);
        const int gz = floorDivInt(static_cast<int>(std::floor(z)), kSeamountCell);
        for (int oz = -1; oz <= 1; ++oz) {
            for (int ox = -1; ox <= 1; ++ox) {
                const int cx = gx + ox, cz = gz + oz;
                if (rand01(cx, 1, cz, s) > 0.4f) continue;
                const float px = (static_cast<float>(cx) + 0.2f + 0.6f * rand01(cx, 2, cz, s)) * kSeamountCell;
                const float pz = (static_cast<float>(cz) + 0.2f + 0.6f * rand01(cx, 3, cz, s)) * kSeamountCell;
                const float r = 28.0f + rand01(cx, 4, cz, s) * 52.0f;
                const float d = std::hypot(x - px, z - pz);
                if (d >= r) continue;
                // Altura de la cima: casi siempre bajo el agua; alguno asoma (isla volcanica).
                const float roll = rand01(cx, 5, cz, s);
                const float top = roll < 0.12f ? sea + 3.0f + roll * 40.0f : sea - 4.0f - rand01(cx, 6, cz, s) * 26.0f;
                const float k = 1.0f - d / r;
                const float shape = k * k * (3.0f - 2.0f * k);
                const float m = lerp(y, std::max(y, top), shape * abyss);
                if (m > y) {
                    y = m;
                    seabed = y < sea - 22.0f ? 1 : 0;  // laderas de basalto; la cima, sedimento (arrecifes)
                }
            }
        }
    }
    return std::max(y, 5.0f);  // por encima de la roca madre
}

BlockId Generator::seabedBlock(const AdventureColumn& c, int x, int z, int depth) const {
    const int wd = sea_ - 1 - c.height;  // agua encima del suelo
    if (c.seabed != 0) return depth < 4 ? block::Basalt : block::Stone;
    if (wd >= 34) return depth < 2 ? block::SeaSilt : block::Clay;  // fondo abisal oscuro
    const float n = fbm2(static_cast<float>(x) / 28.0f, static_cast<float>(z) / 28.0f, 2, 406);
    if (wd >= 14) return n < -0.18f ? block::Gravel : (n > 0.22f ? block::Clay : block::Sand);
    if (c.temperature > 0.2f) return block::Sand;
    return n < -0.1f ? block::Gravel : (n > 0.25f ? block::Clay : block::Sand);
}

int Generator::oceanDepth(int x, int z) const {
    if (profile_ == VoxelGenerator::Adventure) {
        const AdventureColumn c = adventureColumn(x, z);
        if (c.lake || c.biome == AB::ARiver || c.water != sea_ || c.height >= sea_ - 1) return 0;
        return std::max(0, sea_ - 1 - terrainHeight(x, z));
    }
    if (profile_ == VoxelGenerator::Classic) return std::max(0, sea_ - 1 - terrainHeight(x, z));
    return 0;
}

Generator::AdventureColumn Generator::adventureColumn(int x, int z) const {
    AdventureColumn c = adventureColumnBase(x, z);
    c.water = c.height < sea_ ? sea_ : 0;
    if (c.river_level > 0 && c.height < c.river_level - 1) c.water = c.river_level;  // rio por encima del mar
    if (c.pond > c.height) c.water = std::max(c.water, c.pond);  // NOCHE-MUNDO: charcas y estanques
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
    // Lejos del origen, oceanos grandes (oceanBasin = 0 cerca: el de siempre).
    // OCEANOS-HZ (GeneratorOcean.cpp): fuera de las regiones del origen, continentes y oceanos de
    // varios km (rampa suave; ow = 0 dentro: el de siempre bloque a bloque).
    const float ow = oceanScaleWeight(fx, fz);
    float continent = 0.0f;
    if (ow < 1.0f) {
        continent = fbm2(fx / 1300.0f, fz / 1300.0f, 4, 101) + fbm2(fx / 260.0f, fz / 260.0f, 2, 102) * 0.12f + 0.15f - oceanBasin(fx, fz);
    }
    if (ow > 0.0f) continent = lerp(continent, oceanContinent(fx, fz), ow);
    const float land = std::clamp(continent * 2.6f, -1.0f, 1.0f);
    const float hills = fbm2(fx / 150.0f, fz / 150.0f, 4, 103);
    const float detail = fbm2(fx / 38.0f, fz / 38.0f, 2, 104);
    const float ridge = 1.0f - std::abs(fbm2(fx / 420.0f, fz / 420.0f, 4, 105));
    const float mountain_mask = std::clamp((fbm2(fx / 800.0f, fz / 800.0f, 3, 106) + 0.02f) * 2.4f, 0.0f, 1.0f);
    const float inland = std::clamp(land * 2.2f, 0.0f, 1.0f);
    float h;
    if (land < 0.0f) {
        h = sea + land * 30.0f + hills * 4.0f;  // fondo marino
        if (ow > 0.0f) h = lerp(h, std::min(h, sea - 1.0f), ow);  // OCEANOS-HZ: la costa es limpia (sin islotes de un bloque)
        // Oceanos hondos lejos del pueblo de inicio (junto a la costa, el fondo de siempre).
        const float w = oceanWeight(fx, fz) * smoothstep(0.0f, 0.035f, -continent);
        if (w > 0.0f) {
            int seabed = 0;
            // OCEANOS-HZ: la plataforma se estira en unos tramos de costa (mares someros anchos).
            const float e = ow > 0.0f ? -continent * lerp(1.0f, oceanShelf(fx, fz), ow) : -continent;
            const float deep = oceanFloor(fx, fz, e, hills, seabed);
            h = lerp(h, deep, w);
            c.ocean = w;
            c.seabed = w > 0.5f ? seabed : 0;
        }
    } else {
        h = sea + 2.0f + land * 12.0f + hills * 14.0f * inland + detail * 2.5f * inland +
            std::pow(ridge, 2.6f) * 105.0f * mountain_mask * inland;
        if (ow > 0.0f) h = lerp(h, oceanCoast(fx, fz, continent, h), ow);  // OCEANOS-HZ: playas y acantilados
    }
    // Tierras salvajes (GeneratorWild.cpp; nada a menos de ~1,4 km del origen):
    // la provincia y las cordilleras gigantes (antes del clima: las cumbres enfrian).
    wildShape(fx, fz, land, inland, 0, h, c);
    // NATURALEZA-HZ (GeneratorNature.cpp): cordilleras masivas, valles y mesetas (0 dentro de las
    // regiones del origen; entra con una rampa suave; se apaga dentro de las provincias salvajes).
    // Antes del clima: las cumbres enfrian y nievan. El ruido de los rios se calcula una vez (es el
    // mismo que usan los rios de mas abajo).
    // OCEANOS-HZ: fuera de las regiones del origen, rios mas escasos y estrechos (GeneratorOcean.cpp).
    const auto riverNoise = [&]() {
        if (ow >= 1.0f) return std::abs(oceanRiverNoise(fx, fz));
        const float n = fbm2(fx / 600.0f, fz / 600.0f, 3, 110) + fbm2(fx / 150.0f, fz / 150.0f, 2, 111) * 0.07f;
        return std::abs(ow > 0.0f ? lerp(n, oceanRiverNoise(fx, fz), ow) : n);
    };
    float rv_cached = -1.0f;
    if (land > 0.0f && nature_ && natureWeight(fx, fz) > 0.0f) {
        rv_cached = riverNoise();
        h = natureRelief(fx, fz, land, inland, rv_cached, h, c);
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
    // Biomas peligrosos: Tierras Carmesi, Desierto Rosa, Paramo de Ceniza, Tierras Marchitas.
    if (c.region >= WBadlands) wildShape(fx, fz, land, inland, 1, h, c);

    // Rios: donde un ruido de baja frecuencia pasa por cero. Valle ancho y cauce estrecho.
    if (land > 0.0f) {
        const float rv = rv_cached >= 0.0f ? rv_cached : riverNoise();
        const float width = 0.022f + 0.012f * std::clamp(c.humidity * 0.5f + 0.5f, 0.0f, 1.0f);
        c.river = (1.0f - smoothstep(width * 0.6f, width * 1.8f, rv)) * std::clamp(land * 6.0f, 0.0f, 1.0f);
        const float rw = oceanWeight(fx, fz);  // 0 cerca del pueblo de inicio: el rio de siempre
        if (rw <= 0.0f) {
            const float valley = (1.0f - smoothstep(width, width * 7.0f, rv)) * std::clamp(land * 6.0f, 0.0f, 1.0f);
            const float floodplain = sea + 2.0f + (h - sea - 2.0f) * 0.3f;
            h = lerp(h, std::min(h, floodplain), valley * 0.7f);
            h = lerp(h, std::min(h, sea - 3.0f), smoothstep(0.0f, 1.0f, c.river));
        } else {
            // Rios de verdad: el agua va por encima del nivel del mar y baja hacia la costa por
            // escalones de 4 bloques (cascadas y rapidos donde cambia; generateAdventure pone
            // la cortina de agua), mas altos en las montanas (gargantas con saltos). Se
            // ensanchan hacia la desembocadura (estuarios) y el cauce es mas hondo abajo.
            const float wide = 1.0f + 1.6f * (1.0f - smoothstep(0.0f, 0.35f, land)) * rw;
            const float wd = width * wide;
            const float near_coast = std::clamp(land * 6.0f, 0.0f, 1.0f);
            c.river = (1.0f - smoothstep(wd * 0.6f, wd * 1.8f, rv)) * near_coast;
            const float valley = (1.0f - smoothstep(wd, wd * 7.0f, rv)) * near_coast;
            const float mtn = std::pow(ridge, 2.6f) * 105.0f * mountain_mask * inland;
            const float lc = sea + rw * (land * 14.0f + mtn * 0.45f);  // nivel continuo del agua
            const float steps = (lc - sea) / 4.0f;
            const int level = sea_ + 4 * static_cast<int>(std::floor(steps));
            const float frac = steps - std::floor(steps);  // cerca de 1: justo bajo un salto (poza)
            const float depth = 2.5f + 2.5f * (1.0f - smoothstep(0.0f, 0.5f, land)) + 3.0f * smoothstep(0.7f, 1.0f, frac);
            // Valle: llanura de inundacion, nunca por debajo del agua junto al cauce (orillas).
            const float floodplain = std::max(sea + 2.0f + (h - sea - 2.0f) * 0.3f, lc + 2.0f);
            h = lerp(h, std::min(h, floodplain), valley * 0.7f);
            if (valley > 0.0f) h = std::max(h, lerp(h, lc + 2.0f, smoothstep(0.0f, 0.25f, valley)));
            h = lerp(h, std::min(h, static_cast<float>(level) - 1.0f - depth), smoothstep(0.0f, 1.0f, c.river));
            if (c.river > 0.0f && level > sea_) c.river_level = level;
        }
    }
    if (c.region == WAsh || isLushRegion(c.region)) wildShape(fx, fz, land, inland, 2, h, c);  // charcas de lava (NOCHE-MUNDO: y de agua)
    c.height = std::clamp(static_cast<int>(std::lround(h)), 4, (c.giant || c.peak) ? kWorldHeight - 10 : kWorldHeight - 30);
    if (c.feature == 1 || c.feature == 2) {
        c.feature_base = std::min(c.feature_base, c.height);
        if (c.feature_base >= c.height) c.feature = 0;
    }

    // Bioma.
    if (c.height < sea_ - 1) {
        c.biome = c.river > 0.3f ? AB::ARiver : AB::AOcean;
    } else if (c.river > 0.25f && c.height <= (c.river_level > 0 ? c.river_level : sea_) + 2) {
        c.biome = AB::ARiver;
    } else if (c.height <= sea_ + 2 && land < 0.1f) {
        c.biome = c.temperature < -0.45f ? AB::ASnow : AB::ABeach;
    } else if (c.region >= WBadlands && c.strength > 0.45f) {
        c.biome = wildBiome(c);
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
                // Tuneles anchos y cavernas hondas (GeneratorCaves.cpp).
                cave_grid[static_cast<std::size_t>(gx + gz * kGx + gy * kGx * kGx)] =
                    caveDensity(static_cast<float>(x0 + gx * kStep), static_cast<float>(gy * kStep), static_cast<float>(z0 + gz * kStep));
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

    // Entradas de cueva que pasan por este chunk (VoxelWorld::cave_entrances).
    EntranceCarve carve;
    if (cave_entrances_ > 0) {
        std::array<int, kChunkSize * kChunkSize> heights{};
        for (int z = 0; z < kChunkSize; ++z) {
            for (int x = 0; x < kChunkSize; ++x) heights[static_cast<std::size_t>(x + z * kChunkSize)] = col(x, z).height;
        }
        caveEntranceCarve(x0, z0, heights, carve);
    }
    // Cuevas inmensas lejos del pueblo de inicio (GeneratorCaves.cpp): barrancos y pozos abiertos
    // al cielo, lagos y lava, roca honda y menas mas ricas. Cerca del origen, nada (deep = 0).
    std::array<CaveColumn, kChunkSize * kChunkSize> caves;
    caveColumns(x0, z0, cols.data(), kB, caves);

    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            const AdventureColumn& c = col(x, z);
            const int wx = x0 + x, wz = z0 + z;
            const bool underwater = c.water > c.height;  // mar o lago encima
            const int slope = std::max({std::abs(col(x + 1, z).height - c.height), std::abs(col(x - 1, z).height - c.height),
                                        std::abs(col(x, z + 1).height - c.height), std::abs(col(x, z - 1).height - c.height)});
            const bool cliff = slope >= 4 && !underwater;
            const bool lava_pool = c.lava > c.height;  // charca o crater de lava encima (tierras salvajes)
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
            if (isLushBiome(c.biome)) {  // NOCHE-MUNDO: tintes propios (selva intensa, sabana seca, otono...)
                lushTint(c, chunk.grass_tint[static_cast<std::size_t>(x + z * kChunkSize)],
                         chunk.foliage_tint[static_cast<std::size_t>(x + z * kChunkSize)]);
            }

            // Agua de las columnas de al lado (las cuevas no se abren junto a ella).
            const AdventureColumn* side[4] = {&col(x + 1, z), &col(x - 1, z), &col(x, z + 1), &col(x, z - 1)};
            const auto water_beside = [&](int y) {
                for (const AdventureColumn* n : side) {
                    if (y > n->height && y < n->water) return true;
                }
                return false;
            };
            const std::size_t ci = static_cast<std::size_t>(x + z * kChunkSize);
            const bool carved = carve.any && carve.lo[ci] <= carve.hi[ci] && carve.lo[ci] <= c.height;
            const int carve_lo = carve.lo[ci], carve_hi = carve.hi[ci];
            const CaveColumn& cv = caves[ci];
            for (int y = 0; y <= std::min(std::max({c.height, c.water - 1, c.lava - 1}), kWorldHeight - 1); ++y) {
                BlockId id = block::Air;
                if (y <= c.height) {
                    const int depth = c.height - y;
                    if (y == 0 || (y < 4 && rand01(wx, y, wz, seed + 3u) < 0.5f)) {
                        id = block::Bedrock;
                    } else if (c.biome >= AB::ABadlands && (id = wildBlock(c, wx, y, wz, depth, soil, cliff, underwater)) != block::Air) {
                        // Tierras salvajes (GeneratorWild.cpp); aire = la roca de siempre (con sus menas).
                    } else if (depth == 0) {
                        switch (c.biome) {
                            case AB::AOcean:
                                if (c.ocean > 0.0f) id = seabedBlock(c, wx, wz, 0);  // oceano hondo
                                else id = c.height < sea_ - 8 ? block::Gravel : (c.temperature > 0.2f ? block::Sand : block::Clay);
                                break;
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
                                // Cordillera gigante: paredes de roca desnuda y hielo en lo mas alto.
                                if ((c.giant || c.peak) && slope >= 9) id = block::Stone;
                                else if ((c.giant || c.peak) && y > sea_ + 140 && moss_patch > 0.25f) id = block::Ice;
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
                            case AB::AOcean: id = c.ocean > 0.0f ? seabedBlock(c, wx, wz, depth) : (c.height < sea_ - 8 ? block::Gravel : block::Sand); break;
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
                        // Lejos del pueblo de inicio, mas vetas y mas ricas cuanto mas hondo (0 cerca: lo de siempre).
                        const float rich = cv.deep * std::clamp((48.0f - static_cast<float>(y)) / 40.0f, 0.0f, 1.0f);
                        if (vein > 0.36f - rich * 0.06f) {
                            if (static_cast<float>(y) < 16.0f + rich * 8.0f && r < 0.18f + rich * 0.06f) id = block::DiamondOre;
                            else if (static_cast<float>(y) < 32.0f + rich * 6.0f && r < 0.3f + rich * 0.06f) id = block::GoldOre;
                            else if (y < 24 && r < 0.26f) id = block::CrystalOre;
                            else if (arcanite_ore_ && y < 40 && r > 0.9f) id = block::ArcaniteOre;  // senales (Rails.h)
                            else if (y < 64 && r < 0.55f) id = block::IronOre;
                            else if (y >= 40 && y < 110 && r > 0.72f) id = block::CopperOre;
                            else if (y < 128) id = block::CoalOre;
                        }
                        if (id == block::Stone && y < cv.deepstone) id = block::Deepstone;  // roca honda
                    }
                    // Cuevas: no se abren bajo el agua ni (salvo en las entradas) en la capa de arriba,
                    // ni pegadas al agua de la columna de al lado: son secas (el agua son bloques
                    // y solo esta donde el mar o un lago la ponen).
                    if (id != block::Bedrock && y > 4 && cave(x, y, z) && !(underwater && depth < 6) && !(lava_pool && depth < 6) &&
                        !(depth < 3 && !cave_entrance) && !(c.height < sea_ + 3 && depth < 4) && !water_beside(y)) {
                        id = block::Air;
                    }
                    // Barrancos y pozos abiertos al cielo, y el
                    // hueco por debajo del nivel de los lagos y de la lava, lleno (GeneratorCaves.cpp).
                    if (y >= cv.open_lo && id != block::Bedrock && !water_beside(y) && caveOpen(cv, y)) {
                        id = block::Air;
                    }
                    if (id == block::Air && y <= cv.liquid_top) id = cv.liquid;
                    if (carved && y >= carve_lo && y <= carve_hi && id != block::Bedrock && y > 4) id = block::Air;
                } else if (y < c.water) {
                    id = block::Water;
                } else if (y < c.lava) {
                    id = block::Lava;
                }
                chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))] = id;
            }
            // Agua helada en el frio.
            if (underwater && c.temperature < -0.45f && c.water - 1 < kWorldHeight) {
                chunk.blocks[static_cast<std::size_t>(cellIndex(x, c.water - 1, z))] = block::Ice;
            }
            // Cascadas y rapidos: el agua de un rio vecino mas alto cae por este lado hasta el
            // agua (o el suelo) de esta columna. Bloques de cascada quietos (no corren: el agua
            // de bloques solo se mueve si el jugador cambia algo al lado).
            {
                int fall_top = 0;
                for (const AdventureColumn* n : side) {
                    if (n->river_level > 0 && n->water > n->height) fall_top = std::max(fall_top, n->water);
                }
                const int from = std::max(c.height + 1, c.water);
                for (int y = from; y < fall_top && y < kWorldHeight; ++y) {
                    BlockId& cell = chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))];
                    if (cell == block::Air) cell = block::FallingWater;
                }
            }

            if (carved) {
                // Suelo macizo bajo la entrada (sin agujeros de las cuevas de debajo).
                if (carve.flags[ci] & 1) {
                    for (int y = carve_lo - 2; y < carve_lo; ++y) {
                        if (y > 4 && y <= c.height && chunk.get(x, y, z) == block::Air) {
                            chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))] = block::Stone;
                        }
                    }
                }
                // Setas luminosas y brotes de cristal junto a las bocas (se ven de noche).
                if ((carve.flags[ci] & 2) && carve_lo > 1 && carve_lo < kWorldHeight &&
                    blockDef(chunk.get(x, carve_lo - 1, z)).solid && chunk.get(x, carve_lo, z) == block::Air) {
                    chunk.blocks[static_cast<std::size_t>(cellIndex(x, carve_lo, z))] =
                        rand01(wx, carve_lo, wz, seed + 303u) < 0.3f ? block::CrystalSprout : block::GlowMushroom;
                }
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

            // --- Fondo del oceano hondo: arrecifes de coral, bosques de kelp, praderas
            // marinas y anemonas luminosas en lo hondo (solo lejos del pueblo: c.ocean > 0) ---
            if (c.ocean > 0.0f && underwater && c.water == sea_ && c.biome == AB::AOcean && !c.lake) {
                const int floor_y = c.height;
                const int wd = c.water - 1 - floor_y;  // celdas de agua sobre el suelo
                const BlockId ground = chunk.get(x, floor_y, z);
                if (wd < 2 || floor_y + 2 >= kWorldHeight || !blockDef(ground).solid || chunk.get(x, floor_y + 1, z) != block::Water) continue;
                const float fwx = static_cast<float>(wx), fwz = static_cast<float>(wz);
                const float r = rand01(wx, 9, wz, seed + 31u);
                const auto put = [&](int y, BlockId id) {
                    if (y > floor_y && y < c.water - 1) chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))] = id;
                };
                // Arrecife: mar calido y poco hondo.
                const float reef = fbm2(fwx / 40.0f, fwz / 40.0f, 2, 407);
                if (c.temperature > 0.28f && wd >= 3 && wd <= 16 && reef > 0.08f && c.seabed == 0) {
                    const float kind = fbm2(fwx / 9.0f, fwz / 9.0f, 1, 408);
                    const BlockId coral = kind > 0.0f ? block::CoralBlock : block::CoralTube;
                    chunk.blocks[static_cast<std::size_t>(cellIndex(x, floor_y, z))] = coral;
                    // Cabezos: montones de 1-3 bloques.
                    int top_y = floor_y;
                    if (r < 0.45f) {
                        const int mound = 1 + static_cast<int>(rand01(wx, 10, wz, seed + 31u) * 3.0f);
                        for (int i = 1; i <= mound && floor_y + i < c.water - 3; ++i) {
                            put(floor_y + i, rand01(wx, floor_y + i, wz, seed + 37u) < 0.7f ? coral : (coral == block::CoralBlock ? block::CoralTube : block::CoralBlock));
                            top_y = floor_y + i;
                        }
                    }
                    const float p = rand01(wx, 11, wz, seed + 31u);
                    if (p < 0.35f) put(top_y + 1, block::CoralFan);
                    else if (p < 0.5f) put(top_y + 1, block::SeaGrass);
                    continue;
                }
                // Bosque de kelp: mar templado o frio, de 6 a 40 m de hondo.
                const float forest = fbm2(fwx / 55.0f, fwz / 55.0f, 2, 409);
                if (c.temperature < 0.4f && wd >= 6 && wd <= 40 && forest > 0.04f && c.seabed == 0 && r < 0.32f) {
                    const int tall = std::max(2, std::min(wd - 2, static_cast<int>(static_cast<float>(wd) * (0.45f + 0.5f * rand01(wx, 12, wz, seed + 31u)))));
                    for (int i = 1; i <= tall; ++i) put(floor_y + i, block::Kelp);
                    continue;
                }
                // Pradera marina en lo poco hondo.
                if (wd <= 20 && ground == block::Sand && r < (forest > 0.04f ? 0.5f : 0.2f)) {
                    put(floor_y + 1, block::SeaGrass);
                    continue;
                }
                // Lo hondo: anemonas luminosas sobre el limo y en las fosas; respiraderos en la dorsal.
                if (wd >= 30) {
                    if (c.seabed == 1 && r < 0.004f) {
                        chunk.blocks[static_cast<std::size_t>(cellIndex(x, floor_y, z))] = block::Magma;
                    } else if (r < (c.seabed == 2 ? 0.035f : 0.012f)) {
                        put(floor_y + 1, block::SeaGlow);
                    }
                }
                continue;
            }
            // --- Vegetacion sobre el suelo ---
            const int top = c.height;
            // NOCHE-MUNDO: nenufares en las charcas del pantano y los estanques de los cerezos.
            if (underwater && c.pond > c.height && c.water == c.pond && c.water < kWorldHeight && isLushBiome(c.biome) &&
                chunk.get(x, c.water, z) == block::Air &&
                rand01(wx, 13, wz, seed + 9u) < (c.biome == AB::ASwamp ? 0.10f : 0.07f)) {
                chunk.blocks[static_cast<std::size_t>(cellIndex(x, c.water, z))] = block::LilyPad;
                continue;
            }
            if (top + 2 >= kWorldHeight || underwater || lava_pool || chunk.get(x, top + 1, z) != block::Air) continue;
            if (carved && carve_hi >= top) continue;  // la entrada se llevo el suelo
            const BlockId ground = chunk.get(x, top, z);
            if (cv.open_lo <= top && ground == block::Air) continue;  // un barranco o un pozo se llevo el suelo
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
            if ((ground == block::Grass || ground == block::Sand) && c.river > 0.15f && top <= (c.river_level > 0 ? c.river_level : sea_) + 1) {
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
                    // Tierras salvajes.
                    case AB::ABadlands:
                        if (ground == block::RedSand) plant = r < 0.02f ? block::DeadBush : (r < 0.05f ? block::DryGrass : block::Air);
                        break;
                    case AB::ARoseDesert:
                        if (ground == block::PinkSand) plant = r < 0.008f ? block::DeadBush : (r < 0.013f ? block::CrystalSprout : block::Air);
                        break;
                    case AB::AAshlands:
                        if (ground == block::Ash || ground == block::Basalt) {
                            plant = r < 0.03f ? block::EmberGrass : (r < 0.034f ? block::DeadBush : block::Air);
                        }
                        break;
                    case AB::ABlight:
                        if (ground == block::BlightGrass) {
                            plant = r < 0.10f ? block::BlightThorns : (r < 0.12f ? block::DeadBush : (r < 0.13f ? block::Bones : block::Air));
                        }
                        break;
                    default:
                        if (isLushBiome(c.biome)) plant = lushPlant(c, ground, wx, wz, r);  // NOCHE-MUNDO
                        break;
                }
            }
            if (plant == block::Air) continue;
            chunk.blocks[static_cast<std::size_t>(cellIndex(x, top + 1, z))] = plant;
            // Juncos de dos alturas a veces.
            if (plant == block::Reeds && r < 0.15f && chunk.get(x, top + 2, z) == block::Air) {
                chunk.blocks[static_cast<std::size_t>(cellIndex(x, top + 2, z))] = block::Reeds;
            }
            // NOCHE-MUNDO: hierba alta de la sabana (dos de alto) y helechos dobles de la selva.
            if (((plant == block::DryGrass && c.biome == AB::ASavanna) || (plant == block::Fern && c.biome == AB::AJungle)) &&
                rand01(wx, 14, wz, seed + 9u) < 0.35f && chunk.get(x, top + 2, z) == block::Air) {
                chunk.blocks[static_cast<std::size_t>(cellIndex(x, top + 2, z))] = plant;
            }
        }
    }
    decorateCaves(chunk, caves);  // grutas, espeleotemas y minas de las cuevas inmensas
    placeNatureDecor(chunk);  // NATURALEZA-HZ: afloramientos, rocas, troncos caidos y arbustos (GeneratorNature.cpp)
    placeAdventureTrees(chunk);
    placeLushTrees(chunk);  // NOCHE-MUNDO: arboles de los biomas frondosos (GeneratorLush.cpp)
    // Tierras salvajes: islas flotantes, agujas, respiraderos (GeneratorWild.cpp).
    std::array<int, kChunkSize * kChunkSize> heights{};
    {
        for (int z = 0; z < kChunkSize; ++z) {
            for (int x = 0; x < kChunkSize; ++x) heights[static_cast<std::size_t>(x + z * kChunkSize)] = col(x, z).height;
        }
        placeWild(chunk, heights);
    }
    // Simas abisales (GeneratorDeep.cpp): solo lejos del origen; bajan al Reino Profundo.
    carveSinkholes(chunk, heights);
    // Aldeas, ciudades, torres, ruinas y mazmorras (Structures.cpp). Con las alturas
    // del suelo: el aplanado no confunde una isla flotante con el suelo.
    if (structures_) structures_->stamp(chunk, &heights);
}

// --- Arboles ------------------------------------------------------------------------------

// ARBOLES-HZ: 16 direcciones (vectores unitarios fijos: nada de sin/cos, igual en todas las plataformas).
constexpr float kDir16[16][2] = {{1.0f, 0.0f},        {0.9239f, 0.3827f},   {0.7071f, 0.7071f},   {0.3827f, 0.9239f},
                                 {0.0f, 1.0f},        {-0.3827f, 0.9239f},  {-0.7071f, 0.7071f},  {-0.9239f, 0.3827f},
                                 {-1.0f, 0.0f},       {-0.9239f, -0.3827f}, {-0.7071f, -0.7071f}, {-0.3827f, -0.9239f},
                                 {0.0f, -1.0f},       {0.3827f, -0.9239f},  {0.7071f, -0.7071f},  {0.9239f, -0.3827f}};

// Candidatos en una rejilla de 4 x 4 del mundo con su posicion al azar
// (determinista); se prueban los de alrededor del chunk y se escribe la parte
// que cae dentro (copas de hasta 5 de radio).
void Generator::placeAdventureTrees(ChunkData& chunk) const {
    const int x0 = chunk.cx * kChunkSize;
    const int z0 = chunk.cz * kChunkSize;
    constexpr int kCell = 4;
    constexpr int kReach = 13;  // ARBOLES-HZ: robles gigantes con ramas largas (antes 6)
    const auto put = [&](int wx, int y, int wz, BlockId id, bool only_air) {
        const int lx = wx - x0, lz = wz - z0;
        if (lx < 0 || lz < 0 || lx >= kChunkSize || lz >= kChunkSize || y < 1 || y >= kWorldHeight) return;
        BlockId& cell = chunk.blocks[static_cast<std::size_t>(cellIndex(lx, y, lz))];
        if (only_air && cell != block::Air && !blockDef(cell).replaceable) return;
        cell = id;
    };
    const std::uint32_t s = static_cast<std::uint32_t>(seed_) + 977u;
    const float clear_margin = treeClearMargin();  // franja libre junto a aldeas y caminos
    // Sin arboles en las entradas de cueva (ni tapandolas).
    const auto entrances = entrancesTouching(x0 - kReach, z0 - kReach, x0 + kChunkSize + kReach, z0 + kChunkSize + kReach);
    const int gx0 = static_cast<int>(std::floor(static_cast<float>(x0 - kReach) / kCell));
    const int gx1 = static_cast<int>(std::floor(static_cast<float>(x0 + kChunkSize + kReach) / kCell));
    const int gz0 = static_cast<int>(std::floor(static_cast<float>(z0 - kReach) / kCell));
    const int gz1 = static_cast<int>(std::floor(static_cast<float>(z0 + kChunkSize + kReach) / kCell));
    for (int gz = gz0; gz <= gz1; ++gz) {
        for (int gx = gx0; gx <= gx1; ++gx) {
            const int tx = gx * kCell + static_cast<int>(rand01(gx, 1, gz, s) * kCell);
            const int tz = gz * kCell + static_cast<int>(rand01(gx, 2, gz, s) * kCell);
            const AdventureColumn c = adventureColumn(tx, tz);
            if (c.height < sea_ + 1 || c.height > kWorldHeight - 30 || c.river > 0.2f || c.lake || c.lava > c.height || c.feature != 0) continue;
            if (structures_ && structures_->blocksTrees(tx, tz, clear_margin)) continue;  // sin arboles en aldeas y ciudades
            if (!entrances.empty() && nearCaveEntrance(entrances, tx, tz, 2.0f)) continue;
            if (caveOpenNear(tx, tz)) continue;  // ni en los barrancos ni en los pozos
            float chance = 0.0f;
            switch (c.biome) {
                case AB::AForest: chance = 0.55f; break;
                case AB::AMeadow: chance = 0.035f; break;
                case AB::ASnow: chance = 0.22f; break;
                case AB::AMountains: chance = c.height < sea_ + 66 ? 0.14f : 0.0f; break;
                case AB::ADesert: chance = c.mesa ? 0.02f : 0.06f; break;  // cactus
                case AB::ABeach: chance = 0.07f; break;                    // palmeras
                case AB::ABadlands: chance = 0.012f; break;                // cactus
                case AB::AAshlands: chance = 0.02f; break;                 // arboles calcinados
                case AB::ABlight: chance = 0.06f; break;                   // arboles muertos
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

            if (c.biome == AB::AAshlands || c.biome == AB::ABlight) {
                // Arbol muerto: tronco y unas ramas desnudas que suben en diagonal.
                const BlockId log = c.biome == AB::AAshlands ? block::SpruceLog : block::BirchLog;
                const int h = 5 + static_cast<int>(size * 5.0f);
                for (int i = 0; i < h; ++i) put(tx, base + i, tz, log, false);
                const int branches = 2 + static_cast<int>(kind * 3.0f);
                for (int b = 0; b < branches; ++b) {
                    const int dir = static_cast<int>(rand01(gx, 10 + b, gz, s) * 4.0f) & 3;
                    const int dx = dir == 0 ? 1 : (dir == 1 ? -1 : 0), dz = dir == 2 ? 1 : (dir == 3 ? -1 : 0);
                    const int y0 = base + h - 1 - static_cast<int>(rand01(gx, 20 + b, gz, s) * 3.0f);
                    const int length = 2 + static_cast<int>(rand01(gx, 30 + b, gz, s) * 2.0f);
                    // RAMAS-HZ: fuera de las regiones del origen, ramas finas (block::Branch).
                    const BlockId twig = natureNew(tx, tz) ? block::Branch : log;
                    for (int i = 1; i <= length; ++i) put(tx + dx * i, y0 + i / 2, tz + dz * i, twig, true);
                }
                continue;
            }
            // NATURALEZA-HZ: fuera de las regiones del origen (Generator::natureNew) arboles y cactus nuevos.
            const bool nuevo = natureNew(tx, tz);
            if (nuevo && (c.biome == AB::ADesert || c.biome == AB::ABadlands)) {
                const auto q = [&](int k) { return rand01(gx, 300 + k, gz, s); };
                static constexpr int kAx[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
                if (kind < 0.45f) {
                    // Saguaro: tronco alto con 1-3 brazos que salen en codo y suben; a veces en flor.
                    const int h = 5 + static_cast<int>(size * 5.0f);
                    for (int i = 0; i < h; ++i) put(tx, base + i, tz, block::Cactus, true);
                    const int arms = std::min(1 + static_cast<int>(q(0) * 3.0f), h >= 6 ? 3 : 2);
                    const int first = static_cast<int>(q(1) * 4.0f) & 3;
                    for (int a = 0; a < arms; ++a) {
                        const int d = (first + a * (arms == 2 ? 2 : 1)) & 3;
                        const int y0 = base + 2 + static_cast<int>(q(2 + a) * static_cast<float>(std::max(1, h - 5)));
                        const int out = q(5 + a) < 0.4f ? 2 : 1;  // codo corto o largo
                        for (int o = 1; o <= out; ++o) put(tx + kAx[d][0] * o, y0, tz + kAx[d][1] * o, block::Cactus, true);
                        const int ax = tx + kAx[d][0] * out, az = tz + kAx[d][1] * out;
                        const int up = std::min(2 + static_cast<int>(q(8 + a) * 3.0f), base + h - 1 - y0);
                        for (int u = 1; u <= up; ++u) put(ax, y0 + u, az, block::Cactus, true);
                        if (q(11 + a) < 0.25f) put(ax, y0 + up + 1, az, block::YellowFlower, true);
                    }
                    if (q(14) < 0.35f) put(tx, base + h, tz, q(15) < 0.5f ? block::RedFlower : block::YellowFlower, true);
                } else if (kind < 0.75f) {
                    // Grupo de cactus bajos (1-3) alrededor del sitio.
                    const int n = 2 + static_cast<int>(q(20) * 3.0f);
                    for (int i = 0; i < n; ++i) {
                        const int ox = i == 0 ? 0 : static_cast<int>(q(21 + i) * 5.0f) - 2;
                        const int oz = i == 0 ? 0 : static_cast<int>(q(26 + i) * 5.0f) - 2;
                        if (i != 0 && ox == 0 && oz == 0) continue;
                        const int b = i == 0 ? base : adventureColumn(tx + ox, tz + oz).height + 1;
                        if (std::abs(b - base) > 1) continue;
                        const int hh = 1 + static_cast<int>(q(31 + i) * 3.0f);
                        for (int k = 0; k < hh; ++k) put(tx + ox, b + k, tz + oz, block::Cactus, true);
                        if (q(36 + i) < 0.2f) put(tx + ox, b + hh, tz + oz, block::RedFlower, true);
                    }
                } else {
                    // Chumbera: palas planas de 2 x 2 que salen una de otra en diagonal, con tunas (flores) en el borde.
                    const int axis = q(40) < 0.5f ? 0 : 1;  // plano de las palas: x-y o z-y
                    put(tx, base, tz, block::Cactus, true);
                    int cx = tx, cy = base + 1, cz = tz;
                    const int pads = 2 + static_cast<int>(q(41) * 2.0f);
                    for (int p = 0; p < pads; ++p) {
                        const int side = q(42 + p) < 0.5f ? -1 : 1;
                        const int ux = axis == 0 ? 1 : 0, uz = axis == 0 ? 0 : 1;
                        for (int dy = 0; dy < 2; ++dy) {
                            for (int w = 0; w < 2; ++w) put(cx + ux * w * side, cy + dy, cz + uz * w * side, block::Cactus, true);
                        }
                        if (q(46 + p) < 0.5f) put(cx + ux * side, cy + 2, cz + uz * side, block::RedFlower, true);
                        // La siguiente pala sale del borde de esta, mas arriba y hacia un lado.
                        cx += ux * side * (p % 2 == 0 ? 1 : 2);
                        cz += uz * side * (p % 2 == 0 ? 1 : 2);
                        cy += 2;
                        if (cy > base + 6) break;
                    }
                }
                continue;
            }
            if (c.biome == AB::ADesert || c.biome == AB::ABadlands) {
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
            // ARBOLES-HZ: arboles organicos (tronco que se estrecha con raices, ramas con su propio racimo de hojas,
            // copas aplanadas con hojas que cuelgan del borde, abetos por capas caidas, palmeras curvas). Solo
            // aritmetica, sqrt y tablas (nada de sin/cos/pow ni de la libreria de azar): igual en todas las plataformas.
            const auto r01 = [&](int k) { return rand01(gx, 100 + k, gz, s); };
            // Racimo de hojas: elipsoide con la cara de arriba en cupula y la de abajo mas plana; borde roto y
            // hojas que cuelgan por debajo del contorno.
            const auto racimo = [&](int cx, int cy, int cz, float r, float ry_up, float ry_down, BlockId leaves, std::uint32_t salt) {
                const int ri = static_cast<int>(std::ceil(r));
                const int up = static_cast<int>(std::ceil(ry_up)), down = static_cast<int>(std::ceil(ry_down));
                for (int dy = -down; dy <= up; ++dy) {
                    const float ry = dy >= 0 ? ry_up : ry_down;
                    for (int dz = -ri; dz <= ri; ++dz) {
                        for (int dx = -ri; dx <= ri; ++dx) {
                            const float d = static_cast<float>(dx * dx + dz * dz) / (r * r) + static_cast<float>(dy * dy) / (ry * ry);
                            if (d > 1.0f) continue;
                            if (d > 0.62f && rand01(cx + dx, cy + dy, cz + dz, s + salt) < 0.25f + (d - 0.62f)) continue;
                            put(cx + dx, cy + dy, cz + dz, leaves, true);
                        }
                    }
                }
                // Hojas que cuelgan del borde de abajo (1 o 2 bloques)
                for (int dz = -ri; dz <= ri; ++dz) {
                    for (int dx = -ri; dx <= ri; ++dx) {
                        const float q = static_cast<float>(dx * dx + dz * dz) / (r * r);
                        if (q < 0.35f || q > 0.92f) continue;
                        const float hc = rand01(cx + dx, cy - 7, cz + dz, s + salt + 3u);
                        if (hc > 0.22f) continue;
                        const int bottom = cy - static_cast<int>(std::floor(ry_down * std::sqrt(1.0f - q)));
                        put(cx + dx, bottom - 1, cz + dz, leaves, true);
                        if (hc < 0.09f) put(cx + dx, bottom - 2, cz + dz, leaves, true);
                    }
                }
            };
            // Desplazamiento del tronco a la altura i (inclinacion que crece hacia arriba).
            const auto desvio = [&](int i, int h, int dir, float lean, int& ox, int& oz) {
                const float k = static_cast<float>(i) / static_cast<float>(std::max(1, h - 1));
                ox = static_cast<int>(std::lround(kDir16[dir][0] * lean * k * k));
                oz = static_cast<int>(std::lround(kDir16[dir][1] * lean * k * k));
            };
            // Tronco (2 x 2 hasta `grosor_hasta`). Devuelve la x / z de arriba.
            const auto tronco = [&](int h, int dir, float lean, BlockId log, int grosor_hasta, int& topx, int& topz) {
                // RAMAS-HZ: fuera del origen la punta del tronco (1 bloque, 2 si pasa de 10) es rama: 8/16 y 4/16.
                const int afina = nuevo && grosor_hasta < h && h >= 6 ? (h >= 10 ? 2 : 1) : 0;
                for (int i = 0; i < h; ++i) {
                    int ox = 0, oz = 0;
                    desvio(i, h, dir, lean, ox, oz);
                    put(tx + ox, base + i, tz + oz, i >= h - afina && i >= grosor_hasta ? block::Branch : log, false);
                    if (i < grosor_hasta) {
                        put(tx + ox + 1, base + i, tz + oz, log, false);
                        put(tx + ox, base + i, tz + oz + 1, log, false);
                        put(tx + ox + 1, base + i, tz + oz + 1, log, false);
                    }
                    topx = tx + ox;
                    topz = tz + oz;
                }
            };
            // Raices: salen del pie hacia fuera y se hunden en el suelo.
            const auto raices = [&](int n, int largo, BlockId log, bool ancho) {
                const int first = static_cast<int>(r01(1) * 16.0f);
                for (int i = 0; i < n; ++i) {
                    const int dir = (first + i * 16 / n + static_cast<int>(r01(2 + i) * 3.0f)) & 15;
                    const int len = 1 + static_cast<int>(r01(10 + i) * static_cast<float>(largo));
                    const int ox = ancho && kDir16[dir][0] > 0.3f ? 1 : 0, oz = ancho && kDir16[dir][1] > 0.3f ? 1 : 0;
                    for (int j = 1; j <= len; ++j) {
                        const int x = tx + ox + static_cast<int>(std::lround(kDir16[dir][0] * static_cast<float>(j)));
                        const int z = tz + oz + static_cast<int>(std::lround(kDir16[dir][1] * static_cast<float>(j)));
                        if (nuevo) {
                            // RAMAS-HZ: raices finas tendidas sobre el suelo (block::Branch; el Mesher les pone el
                            // contrafuerte junto al tronco). Las de los gigantes empiezan con un tronco de 2 de alto.
                            // Se cortan donde el suelo deja de estar a la altura del pie.
                            if (j == 1 && ancho) {
                                put(x, base, z, log, true);
                                put(x, base + 1, z, log, true);
                                continue;
                            }
                            if (j > 1 && adventureColumn(x, z).height != base - 1) break;
                            put(x, base, z, block::Branch, true);
                            continue;
                        }
                        put(x, j == 1 ? base : base - 1, z, log, j != 1 ? false : true);
                        if (j == 1 && ancho) put(x, base + 1, z, log, true);
                    }
                }
            };
            // Rama: de (x, y, z) hacia dir16, `len` en horizontal subiendo `rise`. Devuelve la punta.
            const auto rama = [&](int x, int y, int z, int dir, int len, int rise, BlockId log, int& ex, int& ey, int& ez) {
                ex = x;
                ey = y;
                ez = z;
                for (int j = 1; j <= len; ++j) {
                    ex = x + static_cast<int>(std::lround(kDir16[dir][0] * static_cast<float>(j)));
                    ez = z + static_cast<int>(std::lround(kDir16[dir][1] * static_cast<float>(j)));
                    ey = y + (j * rise + len / 2) / len;
                    put(ex, ey, ez, nuevo ? block::Branch : log, false);  // RAMAS-HZ: rama fina fuera del origen
                }
            };

            if (c.biome == AB::ABeach) {
                // Palmera: tronco que se curva y hojas largas en arco que caen por las puntas.
                const int h = 6 + static_cast<int>(size * 4.0f);
                const int dir = static_cast<int>(kind * 16.0f) & 15;
                int px = tx, pz = tz;
                tronco(h, dir, 2.0f + size * 1.5f, block::OakLog, 0, px, pz);
                const int top = base + h;
                put(px, top, pz, block::JungleLeaves, true);
                static constexpr int kArco[6] = {1, 1, 0, 0, -1, -2};
                const int hojas = 6 + static_cast<int>(r01(40) * 2.0f);
                const int giro = static_cast<int>(r01(41) * 16.0f);
                for (int f = 0; f < hojas; ++f) {
                    const int d = (giro + f * 16 / hojas) & 15;
                    const int largo = 4 + static_cast<int>(r01(42 + f) * 2.0f);
                    for (int i = 1; i <= largo; ++i) {
                        const int x = px + static_cast<int>(std::lround(kDir16[d][0] * static_cast<float>(i)));
                        const int z = pz + static_cast<int>(std::lround(kDir16[d][1] * static_cast<float>(i)));
                        const int y = top + kArco[std::min(i - 1, 5)];
                        put(x, y, z, block::JungleLeaves, true);
                        if (i >= 2 && i <= 3) {  // la hoja es mas ancha en el medio
                            const int p = (d + 4) & 15;
                            put(x + static_cast<int>(std::lround(kDir16[p][0])), y, z + static_cast<int>(std::lround(kDir16[p][1])),
                                block::JungleLeaves, true);
                        }
                    }
                }
                continue;
            }
            if (c.biome == AB::ASnow || c.biome == AB::AMountains) {
                // Abeto por capas: faldas de ramas que se estrechan hacia arriba y caen por el borde.
                const bool grande = size > 0.9f;
                const int h = grande ? 17 + static_cast<int>(kind * 5.0f) : 9 + static_cast<int>(size * 6.0f);
                int px = tx, pz = tz;
                tronco(h, static_cast<int>(kind * 16.0f) & 15, grande ? 0.0f : 0.6f, block::SpruceLog, grande ? h / 2 : 0, px, pz);
                const float max_r = grande ? 5.2f : 2.8f + size * 1.4f;
                const int inicio = grande ? 4 : 2 + static_cast<int>(r01(50) * 2.0f);
                int y = base + inicio;
                int capa = 0;
                while (y < base + h - 1) {
                    const float k = static_cast<float>(y - base - inicio) / static_cast<float>(std::max(1, h - inicio));
                    const float r = (1.0f - k) * max_r + 0.7f;
                    const int ri = static_cast<int>(std::ceil(r));
                    int ox = 0, oz = 0;
                    desvio(y - base, h, static_cast<int>(kind * 16.0f) & 15, grande ? 0.0f : 0.6f, ox, oz);
                    const int cxi = tx + ox, czi = tz + oz;
                    const float cx = static_cast<float>(cxi) + (grande ? 0.5f : 0.0f), cz = static_cast<float>(czi) + (grande ? 0.5f : 0.0f);
                    for (int dz = -ri - 1; dz <= ri + 1; ++dz) {
                        for (int dx = -ri - 1; dx <= ri + 1; ++dx) {
                            const float fx = static_cast<float>(cxi + dx) - cx, fz = static_cast<float>(czi + dz) - cz;
                            const float d = std::sqrt(fx * fx + fz * fz);
                            if (d > r) continue;
                            const float n = rand01(cxi + dx, y, czi + dz, s + 21u);
                            if (d > r - 1.0f && n < 0.3f) continue;  // borde roto
                            put(cxi + dx, y, czi + dz, block::SpruceLeaves, true);
                            if (d > r - 1.3f && n > 0.45f) put(cxi + dx, y - 1, czi + dz, block::SpruceLeaves, true);  // cae
                            if (d < r * 0.55f) put(cxi + dx, y + 1, czi + dz, block::SpruceLeaves, true);               // cono
                        }
                    }
                    y += (capa % 3 == 2) ? 3 : 2;
                    ++capa;
                }
                for (int i = 0; i < 3; ++i) put(px, base + h - 1 + i, pz, block::SpruceLeaves, true);  // punta
                if (grande) raices(5, 3, block::SpruceLog, true);
                put(tx, base - 1, tz, block::Dirt, false);
                continue;
            }
            // NATURALEZA-HZ: especies nuevas (solo fuera de las regiones del origen).
            // Rama con horquilla: la rama, un racimo en la punta y (a veces) una ramita que sale de lado con el suyo.
            const auto ramaHorquilla = [&](int x, int y, int z, int d, int len, int rise, BlockId log, BlockId leaves, float rr, std::uint32_t salt,
                                           int k) {
                int ex = 0, ey = 0, ez = 0;
                rama(x, y, z, d, len, rise, log, ex, ey, ez);
                racimo(ex, ey + 1, ez, rr, rr * 0.6f, rr * 0.4f, leaves, salt);
                if (r01(150 + k) < 0.55f && len >= 3) {
                    const int mid = len * 2 / 3;
                    const int mx = x + static_cast<int>(std::lround(kDir16[d][0] * static_cast<float>(mid)));
                    const int mz = z + static_cast<int>(std::lround(kDir16[d][1] * static_cast<float>(mid)));
                    const int my = y + (mid * rise + len / 2) / len;
                    const int d2 = (d + (r01(160 + k) < 0.5f ? 3 : 13)) & 15;
                    int fx = 0, fy = 0, fz = 0;
                    rama(mx, my, mz, d2, 2, 1, log, fx, fy, fz);
                    racimo(fx, fy + 1, fz, rr * 0.7f, rr * 0.45f, rr * 0.3f, leaves, salt + 7u);
                }
            };
            // Copa de varios lobulos: el racimo central y 2-4 alrededor, a distinta altura.
            const auto copaLobulos = [&](int cx, int cy, int cz, float r, BlockId leaves, std::uint32_t salt) {
                racimo(cx, cy, cz, r * 0.85f, r * 0.55f, r * 0.35f, leaves, salt);
                const int n = 2 + static_cast<int>(r01(170) * 3.0f);
                const int giro = static_cast<int>(r01(171) * 16.0f);
                for (int i = 0; i < n; ++i) {
                    const int d = (giro + i * 16 / n + static_cast<int>(r01(172 + i) * 2.0f)) & 15;
                    const float dist = r * (0.5f + 0.25f * r01(176 + i));
                    const int lx = cx + static_cast<int>(std::lround(kDir16[d][0] * dist));
                    const int lz = cz + static_cast<int>(std::lround(kDir16[d][1] * dist));
                    const int ly = cy - 1 + static_cast<int>(r01(180 + i) * 2.5f);
                    const float lr = r * (0.55f + 0.15f * r01(184 + i));
                    racimo(lx, ly, lz, lr, lr * 0.6f, lr * 0.4f, leaves, salt + 11u + static_cast<std::uint32_t>(i));
                }
            };
            if (nuevo && (c.biome == AB::AForest || c.biome == AB::AMeadow)) {
                // Sauce: tierras bajas y humedas (junto a rios y lagos). Tronco corto y grueso, copa ancha
                // y cortinas de hojas que caen hasta casi el suelo.
                if (c.height <= sea_ + 5 && c.humidity > 0.1f && kind < 0.55f) {
                    const int h = 5 + static_cast<int>(size * 3.0f);
                    const int dir = static_cast<int>(r01(200) * 16.0f) & 15;
                    int px = 0, pz = 0;
                    desvio(h - 1, h, dir, 1.0f, px, pz);
                    px += tx;
                    pz += tz;
                    const float r = 3.6f + size * 1.2f;
                    racimo(px, base + h, pz, r, r * 0.45f, r * 0.3f, block::OakLeaves, 210u);
                    for (int b = 0; b < 4; ++b) {
                        const int d = (dir + 2 + b * 4 + static_cast<int>(r01(201 + b) * 2.0f)) & 15;
                        int ex = 0, ey = 0, ez = 0;
                        rama(tx, base + h - 2, tz, d, 3, 1, block::OakLog, ex, ey, ez);
                    }
                    // Cortinas: en el borde de la copa, columnas de hojas de 2 a 6.
                    const int ri = static_cast<int>(std::ceil(r)) + 1;
                    for (int dz = -ri; dz <= ri; ++dz) {
                        for (int dx = -ri; dx <= ri; ++dx) {
                            const float d2 = static_cast<float>(dx * dx + dz * dz);
                            if (d2 < (r - 1.4f) * (r - 1.4f) || d2 > (r + 0.6f) * (r + 0.6f)) continue;
                            const float hc = rand01(px + dx, 5, pz + dz, s + 220u);
                            if (hc > 0.6f) continue;
                            const int len = 2 + static_cast<int>(hc * 7.0f);
                            for (int i = 0; i < len && base + h - 1 - i > base; ++i) put(px + dx, base + h - 1 - i, pz + dz, block::OakLeaves, true);
                        }
                    }
                    tronco(h, dir, 1.0f, block::OakLog, 2, px, pz);
                    raices(4, 2, block::OakLog, true);
                    put(tx, base - 1, tz, block::Dirt, false);
                    continue;
                }
                // Acacia: pradera calida y seca. Tronco que se abre en 2-3 brazos inclinados con copas planas.
                if (c.biome == AB::AMeadow && c.temperature > 0.2f && c.humidity < 0.05f) {
                    const int h = 3 + static_cast<int>(size * 2.0f);
                    for (int i = 0; i < h; ++i) put(tx, base + i, tz, block::AcaciaLog, false);
                    const int arms = 2 + static_cast<int>(r01(230) * 2.0f);
                    const int giro = static_cast<int>(r01(231) * 16.0f);
                    for (int a = 0; a < arms; ++a) {
                        const int d = (giro + a * 16 / arms + static_cast<int>(r01(232 + a) * 2.0f)) & 15;
                        const int len = 2 + static_cast<int>(r01(236 + a) * 3.0f);
                        int ex = tx, ey = base + h - 1, ez = tz;
                        for (int j = 1; j <= len; ++j) {  // diagonal: sube 1 por cada paso
                            ex = tx + static_cast<int>(std::lround(kDir16[d][0] * static_cast<float>(j)));
                            ez = tz + static_cast<int>(std::lround(kDir16[d][1] * static_cast<float>(j)));
                            ey = base + h - 1 + j;
                            put(ex, ey, ez, block::AcaciaLog, false);
                        }
                        const float rr = 2.4f + r01(240 + a) * 1.3f;
                        racimo(ex, ey + 1, ez, rr, 0.9f, 0.6f, block::AcaciaLeaves, 250u + static_cast<std::uint32_t>(a));
                    }
                    raices(3, 1, block::AcaciaLog, false);
                    put(tx, base - 1, tz, block::Dirt, false);
                    continue;
                }
                // Arbol gigante antiguo (raro, en el bosque): tronco de 2 x 2 con pie ancho, 6-8 ramas con
                // horquilla y una copa enorme de lobulos. Cabe en kReach (13).
                if (c.biome == AB::AForest && kind > 0.9f && size > 0.72f) {
                    const int h = 19 + static_cast<int>(r01(260) * 6.0f);
                    const int dir = static_cast<int>(r01(261) * 16.0f) & 15;
                    copaLobulos(tx, base + h, tz, 6.2f, block::OakLeaves, 270u);
                    const int nr = 6 + static_cast<int>(r01(262) * 3.0f);
                    const int giro = static_cast<int>(r01(263) * 16.0f);
                    for (int b = 0; b < nr; ++b) {
                        const int d = (giro + b * 16 / nr + static_cast<int>(r01(264 + b) * 2.0f)) & 15;
                        const int i = static_cast<int>(static_cast<float>(h) * (0.45f + 0.4f * r01(274 + b)));
                        const int len = 4 + static_cast<int>(r01(284 + b) * 3.0f);
                        // Desde la cara del tronco 2 x 2 que mira hacia la rama.
                        const int ox = kDir16[d][0] > 0.3f ? 1 : 0, oz = kDir16[d][1] > 0.3f ? 1 : 0;
                        ramaHorquilla(tx + ox, base + i, tz + oz, d, len, 2 + static_cast<int>(r01(294 + b) * 2.0f), block::OakLog,
                                      block::OakLeaves, 2.6f + r01(304 + b) * 0.9f, 280u + static_cast<std::uint32_t>(b), b);
                    }
                    int px = tx, pz = tz;
                    tronco(h, dir, 0.0f, block::OakLog, h, px, pz);
                    // Pie ancho (contrafuertes de 3 bloques en cruz) y raices largas.
                    for (int i = 0; i < 3; ++i) {
                        const int up = 3 - i;
                        for (int y = 0; y < up; ++y) {
                            put(tx - 1, base + y, tz + (i & 1), block::OakLog, false);
                            put(tx + 2, base + y, tz + (i & 1), block::OakLog, false);
                            put(tx + (i & 1), base + y, tz - 1, block::OakLog, false);
                            put(tx + (i & 1), base + y, tz + 2, block::OakLog, false);
                        }
                    }
                    raices(8, 4, block::OakLog, true);
                    put(tx, base - 1, tz, block::Dirt, false);
                    continue;
                }
            }
            const bool birch = c.biome == AB::AForest && kind < 0.3f;
            if (birch) {
                // Abedul: fino y alto, algo inclinado, con tres racimos pequenos a distintas alturas.
                const int h = 8 + static_cast<int>(size * 5.0f);
                const int dir = static_cast<int>(r01(60) * 16.0f) & 15;
                const float lean = 0.6f + size;
                int px = tx, pz = tz;
                desvio(h - 1, h, dir, lean, px, pz);
                px += tx;
                pz += tz;
                racimo(px, base + h, pz, 1.8f, 1.7f, 1.2f, block::BirchLeaves, 31u);
                for (int k = 0; k < 2; ++k) {
                    const int d = (dir + 5 + k * 6 + static_cast<int>(r01(61 + k) * 3.0f)) & 15;
                    const int i = h - 3 - k * 2;
                    int ox = 0, oz = 0;
                    desvio(i, h, dir, lean, ox, oz);
                    int ex = 0, ey = 0, ez = 0;
                    rama(tx + ox, base + i, tz + oz, d, 2, 1, block::BirchLog, ex, ey, ez);
                    racimo(ex, ey + 1, ez, 1.9f - 0.2f * static_cast<float>(k), 1.5f, 1.0f, block::BirchLeaves, 32u + static_cast<std::uint32_t>(k));
                }
                tronco(h, dir, lean, block::BirchLog, 0, px, pz);
                raices(3, 1, block::BirchLog, false);
                put(tx, base - 1, tz, block::Dirt, false);
                continue;
            }
            // Roble: tronco que se estrecha, raices, 1-6 ramas con su racimo y una copa ancha aplanada.
            const bool giant = c.biome == AB::AForest && kind > 0.9f;
            const bool medio = !giant && size > 0.55f;
            const int h = giant ? 11 + static_cast<int>(size * 5.0f) : (medio ? 6 + static_cast<int>(size * 3.0f) : 5 + static_cast<int>(size * 2.0f));
            const int dir = static_cast<int>(r01(70) * 16.0f) & 15;
            const float lean = giant ? 0.5f : (r01(71) < 0.5f ? 0.0f : 0.8f + r01(72) * 1.4f);
            int px = 0, pz = 0;
            desvio(h - 1, h, dir, lean, px, pz);
            px += tx;
            pz += tz;
            // Primero los racimos (el tronco y las ramas van despues y los atraviesan).
            const float copa = giant ? 4.4f + size * 1.0f : (medio ? 3.0f + size * 0.8f : 2.4f + size * 0.6f);
            if (nuevo && (giant || medio)) copaLobulos(px, base + h, pz, copa * 1.1f, block::OakLeaves, 41u);  // NATURALEZA-HZ
            else racimo(px, base + h, pz, copa, copa * 0.62f, copa * 0.38f, block::OakLeaves, 41u);
            const int nramas = giant ? 4 + static_cast<int>(r01(73) * 3.0f) : (medio ? 2 + static_cast<int>(r01(73) * 2.0f) : 1 + static_cast<int>(r01(73) * 2.0f));
            const int giro = static_cast<int>(r01(74) * 16.0f);
            for (int b = 0; b < nramas; ++b) {
                const int d = (giro + b * 16 / nramas + static_cast<int>(r01(80 + b) * 3.0f)) & 15;
                const int i = static_cast<int>(static_cast<float>(h) * (0.5f + 0.3f * r01(90 + b)));
                int ox = 0, oz = 0;
                desvio(i, h, dir, lean, ox, oz);
                const int len = giant ? 3 + static_cast<int>(r01(100 + b) * 3.0f) : 2 + static_cast<int>(r01(100 + b) * 2.0f);
                const int rise = 1 + static_cast<int>(r01(110 + b) * 2.0f);
                const float rr = giant ? 2.8f + r01(120 + b) * 1.0f : 1.8f + r01(120 + b) * 0.8f;
                if (nuevo) {  // NATURALEZA-HZ: ramas con horquilla
                    ramaHorquilla(tx + ox, base + i, tz + oz, d, len + 1, rise, block::OakLog, block::OakLeaves, rr, 50u + static_cast<std::uint32_t>(b), b);
                    continue;
                }
                int ex = 0, ey = 0, ez = 0;
                rama(tx + ox, base + i, tz + oz, d, len, rise, block::OakLog, ex, ey, ez);
                racimo(ex, ey + 1, ez, rr, rr * 0.6f, rr * 0.4f, block::OakLeaves, 50u + static_cast<std::uint32_t>(b));
            }
            tronco(h, dir, lean, block::OakLog, giant ? h * 2 / 3 : 0, px, pz);
            raices(giant ? 6 : (medio ? 4 : 3), giant ? 3 : (medio ? 2 : 1), block::OakLog, giant);
            put(tx, base - 1, tz, block::Dirt, false);
        }
    }
}

}  // namespace cramion::voxel
