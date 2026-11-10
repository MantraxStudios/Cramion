// Estructuras del mundo de bloques (Structures.h): planificacion por region,
// estampado por chunk y consultas para los scripts.
//
// Region (kRegionSize = 384 m, la 0,0 centrada en el origen):
//   - La 0,0 tiene SIEMPRE la capital (Kettlecrown): murallas con torres y tres
//     puertas, castillo al norte (salon del trono, biblioteca, armeria,
//     comedor, dormitorio real y calabozos), plaza redonda con fuente y
//     mercado, taberna, herreria, templo, tiendas, establo, muchas casas de
//     2-3 pisos amuebladas y muelle si hay costa cerca. Punto "inicio" en la
//     plaza.
//   - Las demas: a veces una ciudad (rara; lejos, a veces una gran ciudad), un
//     castillo real con su patio de armas (tipo "ciudad") o una aldea (en prados
//     llanos; de playa, nieve, desierto, elfica en el bosque o sobre pilotes en
//     lo humedo, con muelle si hay mar), y ademas mazmorras, torres de mago,
//     ruinas y portales.
//   - Plazas hostiles (desde la segunda corona de regiones; muchas mas en las
//     tierras peligrosas): fuerte de bandidos, campamento de guerra orco,
//     castillo no muerto, ciudad maldita y torre del culto. Dejan puntos
//     "spawner" / "jefe" / "cofre" que puebla el juego (Mazmorras.cpp).
//   - Caminos entre regiones: cada borde puede tener un cruce; la region une sus
//     cruces con la entrada mas cercana de sus asentamientos (RegionPlan::roads).
//   - Todo sale de un generador de azar sembrado con (semilla, region): el
//     mismo plan en cualquier hilo y orden.

#include "Structures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace cramion::voxel {

namespace B = block;

namespace {

constexpr float kPi = 3.14159265f;
constexpr int kHalf = kRegionSize / 2;

std::uint32_t mix32(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

std::uint32_t hashOf(std::uint32_t seed, int a, int b, int c = 0) {
    return mix32(seed ^ mix32(static_cast<std::uint32_t>(a) * 0x9E3779B1u ^ mix32(static_cast<std::uint32_t>(b) * 0x85EBCA77u ^
                                                                                  static_cast<std::uint32_t>(c) * 0xC2B2AE3Du)));
}

int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

std::int64_t regionKey(int rx, int rz) {
    return (static_cast<std::int64_t>(rx) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(rz));
}

// Azar secuencial (xorshift) para planificar.
struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed : 0x1234567u) {}
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float f() { return static_cast<float>(next() & 0xFFFFFF) / 16777216.0f; }
    int range(int lo, int hi) { return lo + static_cast<int>(f() * static_cast<float>(hi - lo + 1)); }
    template <typename T, std::size_t N>
    const T& pick(const T (&list)[N]) { return list[std::min<std::size_t>(N - 1, static_cast<std::size_t>(f() * N))]; }
};

float smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Rect {
    int x0, z0, x1, z1;  // incluidos
    bool overlaps(const Rect& o, int margin) const {
        return !(x1 + margin < o.x0 || o.x1 + margin < x0 || z1 + margin < o.z0 || o.z1 + margin < z0);
    }
};

// Peso del aplanado en (x, z): 1 dentro, 0 fuera, rampa suave entre medias.
float terraformWeight(const Terraform& tf, int x, int z) {
    const float dx = static_cast<float>(x - tf.cx), dz = static_cast<float>(z - tf.cz);
    float d;
    if (tf.square) {
        // Cuadrado con esquinas redondeadas.
        const float ax = std::max(0.0f, std::abs(dx) - tf.inner + 12.0f), az = std::max(0.0f, std::abs(dz) - tf.inner + 12.0f);
        d = tf.inner - 12.0f + std::sqrt(ax * ax + az * az);
    } else {
        d = std::sqrt(dx * dx + dz * dz);
    }
    if (d <= tf.inner) return 1.0f;
    if (d >= tf.outer) return 0.0f;
    return smooth01((tf.outer - d) / (tf.outer - tf.inner));
}

float roadDistance(const Road& r, float px, float pz) {
    const float vx = r.x1 - r.x0, vz = r.z1 - r.z0;
    const float len2 = vx * vx + vz * vz;
    float t = len2 > 0.0f ? ((px - r.x0) * vx + (pz - r.z0) * vz) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return std::hypot(px - (r.x0 + vx * t), pz - (r.z0 + vz * t));
}

// Frente de cada giro (hacia donde mira la fachada local -z).
constexpr int kFront[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};

void toWorld(const Piece& p, int lx, int lz, int& wx, int& wz) {
    const int w = p.plan->w, d = p.plan->d;
    switch (p.rot & 3) {
        case 0: wx = p.x + lx; wz = p.z + lz; break;
        case 1: wx = p.x + (d - 1 - lz); wz = p.z + lx; break;
        case 2: wx = p.x + (w - 1 - lx); wz = p.z + (d - 1 - lz); break;
        default: wx = p.x + lz; wz = p.z + (w - 1 - lx); break;
    }
}

void toLocal(const Piece& p, int wx, int wz, int& lx, int& lz) {
    const int w = p.plan->w, d = p.plan->d;
    const int dx = wx - p.x, dz = wz - p.z;
    switch (p.rot & 3) {
        case 0: lx = dx; lz = dz; break;
        case 1: lz = d - 1 - dx; lx = dz; break;
        case 2: lx = w - 1 - dx; lz = d - 1 - dz; break;
        default: lz = dx; lx = w - 1 - dz; break;
    }
}

bool soilSurface(BlockId id) {
    return id == B::Grass || id == B::Sand || id == B::SnowyGrass || id == B::RedSand || id == B::Moss || id == B::Snow ||
           id == B::Dirt;
}

bool groundLike(BlockId id) {
    if (id == B::Air || id == B::Ice || id == B::OakLog || id == B::BirchLog || id == B::SpruceLog || id == B::Cactus) return false;
    const BlockDef& d = blockDef(id);
    return d.solid && d.opaque && !d.cutout;
}

// --- Nombres (en ingles: los ve el jugador) ---------------------------------------------

std::string villageName(Rng& rng, BuildStyle style) {
    static const char* const kTemperate[] = {"Thimble", "Nettle", "Bramble", "Kettle", "Turnip", "Tansy", "Gorse", "Cider",
                                             "Lantern", "Hobble", "Pickle", "Crumble", "Mossy", "Copper", "Muddle", "Quill",
                                             "Badger", "Plum", "Wobble", "Sorrel", "Barley", "Pudding"};
    static const char* const kSnow[] = {"Mitten", "Sleet", "Icicle", "Woolly", "Hoarfrost", "Shiver", "Bearskin", "Pinecone",
                                        "Snowberry", "Chilblain", "Whitecap"};
    static const char* const kDesert[] = {"Saffron", "Cumin", "Kiln", "Scarab", "Tamarind", "Sunbaked", "Clay", "Jackal",
                                          "Cinnabar", "Date", "Thistle"};
    static const char* const kBeach[] = {"Barnacle", "Driftwood", "Pebble", "Brine", "Gullwing", "Mussel", "Sandpiper", "Winkle",
                                         "Cockle", "Saltlick"};
    static const char* const kSuffix[] = {"wick", "mire", "bottom", "nook", "bridge", "knot", "tump", "worth", "combe", "barrow",
                                          "stile", "holt", "croft", "hollow", "dell", "ditch", "hatch", "end"};
    static const char* const kSeaSuffix[] = {" Wharf", " Cove", " Landing", " Spit", " Quay", "mouth"};
    static const char* const kElven[] = {"Silverleaf", "Moonwhisper", "Thistledown", "Elderglade", "Starfern", "Willowmere",
                                         "Dewlight", "Ashenbough", "Lindenvale", "Glimmerbrook"};
    static const char* const kSwamp[] = {"Bogwater", "Frogmarsh", "Reedholm", "Mirewick", "Murkpool", "Toadstool Ford",
                                         "Sedgewater", "Peatmoss", "Fenlight", "Lilypad Landing"};
    if (style == BuildStyle::Elven) return rng.pick(kElven);
    if (style == BuildStyle::Swamp) return rng.pick(kSwamp);
    std::string a;
    switch (style) {
        case BuildStyle::Snow: a = rng.pick(kSnow); break;
        case BuildStyle::Desert: a = rng.pick(kDesert); break;
        case BuildStyle::Beach: a = rng.pick(kBeach); return a + rng.pick(kSeaSuffix);
        default: a = rng.pick(kTemperate); break;
    }
    return a + rng.pick(kSuffix);
}

std::string cityName(Rng& rng) {
    static const char* const kNames[] = {"Lanternreach", "Saltgrave", "Brassmoor", "Tidebell", "Wyrmgate", "Gildenmaw",
                                         "Copperhollow", "Quarrystone", "Hollowcrown", "Rookspire", "Ninebridges",
                                         "Candlewharf", "Bellhammer", "Greywhistle"};
    static const char* const kForms[] = {"", "Free City of ", "", "Old ", ""};
    return std::string(rng.pick(kForms)) + rng.pick(kNames);
}

std::string dungeonName(Rng& rng) {
    static const char* const kKind[] = {"Crypt", "Catacombs", "Vault", "Halls", "Tomb", "Depths", "Warrens", "Cellars", "Labyrinth"};
    static const char* const kOf[] = {"the Sunken Choir", "the Moth Queen", "a Thousand Teeth", "the Weeping Lantern",
                                      "Rusted Bells", "the Clockwork Saint", "Salt and Bone", "the Gnawing Dark",
                                      "the Hollow Abbot", "Spilled Ink", "the Last Candle", "the Sleeping Mole"};
    return std::string(rng.pick(kKind)) + " of " + rng.pick(kOf);
}

std::string towerName(Rng& rng) {
    static const char* const kMage[] = {"Fennick", "Mirabel", "Quillon", "Ottilie", "Bramwell", "Vesper", "Thaddeus",
                                        "Wendeline", "Corvin", "Hesketh"};
    static const char* const kAdj[] = {"the Ink-Stained", "the Twice-Born", "the Unhurried", "the Moth-Eyed",
                                       "the Absent-Minded", "the Lantern-Bearer", "the Backwards", "the Very Tall"};
    return std::string("Tower of ") + rng.pick(kMage) + " " + rng.pick(kAdj);
}

std::string ruinName(Rng& rng) {
    static const char* const kOld[] = {"Old Tallowmere", "Cindercrook", "Mournwick", "Sallowgate", "Weatherbone",
                                       "Gloamhold", "Pennywhistle Keep", "Crooked Ammon", "Ashcombe"};
    return std::string("Ruins of ") + rng.pick(kOld);
}

std::string castleName(Rng& rng) {
    static const char* const kNames[] = {"Ravenmoor", "Highgarde", "Stormhold", "Aldermere", "Goldcrest", "Wyvernrest",
                                         "Lionsreach", "Thornwall", "Silverkeep", "Dawnspire", "Brightwater", "Oakhelm",
                                         "Falconridge", "Marrowind"};
    static const char* const kForms[] = {"Castle ", "", "Citadel of ", "", "Fort "};
    static const char* const kAfter[] = {"", " Castle", "", " Keep", ""};
    const std::size_t f = std::min<std::size_t>(4, static_cast<std::size_t>(rng.f() * 5.0f));
    return std::string(kForms[f]) + rng.pick(kNames) + kAfter[f];
}

std::string banditName(Rng& rng) {
    static const char* const kA[] = {"Cutthroat", "Blackmask", "Rustblade", "Crowfeather", "Snakebite", "Gallows",
                                     "Dirtwater", "Redcap", "Mudfang", "Knucklebone"};
    static const char* const kB[] = {" Hideout", " Stockade", " Fort", " Den", " Palisade"};
    return std::string(rng.pick(kA)) + rng.pick(kB);
}

std::string orcName(Rng& rng) {
    static const char* const kA[] = {"Skullsplitter", "Bloodtusk", "Ironjaw", "Gutrot", "Bonegnaw", "Ashfang",
                                     "Grimhorn", "Redmaw", "Rotgut", "Blackhide"};
    static const char* const kB[] = {" War Camp", " Warband", " Encampment", " Warcamp"};
    return std::string(rng.pick(kA)) + rng.pick(kB);
}

std::string undeadName(Rng& rng) {
    static const char* const kA[] = {"Dreadmoor", "Gravenholt", "Mourncrest", "Nightspire", "Blackthorn", "Vexhallow",
                                     "Cinderveil", "Grimwald", "Hollowmourn", "Witherkeep"};
    static const char* const kForms[] = {"Castle ", "Fortress of ", "", "The Dead Keep of "};
    return std::string(rng.pick(kForms)) + rng.pick(kA);
}

std::string cursedCityName(Rng& rng) {
    static const char* const kA[] = {"Hollowmere", "Ashenvale", "Saltmourn", "Gravecross", "Duskhaven", "Withermoor",
                                     "Pallid Reach", "Sorrowgate", "Candlemourn", "Greyfall"};
    static const char* const kForms[] = {"Fallen ", "Cursed ", "Old ", "Forsaken "};
    return std::string(rng.pick(kForms)) + rng.pick(kA);
}

std::string cultName(Rng& rng) {
    static const char* const kA[] = {"the Crimson Eye", "the Hollow Star", "a Thousand Whispers", "the Black Sun",
                                     "the Weeping Moon", "the Ninth Gate", "the Ashen Choir", "the Bleeding Saint"};
    return std::string("Spire of ") + rng.pick(kA);
}

// Suelo de una plaza hostil segun el bioma (y el de debajo).
BlockId hostileSurface(int biome) {
    switch (biome) {
        case Generator::ASnow:
        case Generator::AMountains: return B::SnowyGrass;
        case Generator::ADesert:
        case Generator::ABeach: return B::Sand;
        case Generator::ABadlands: return B::RedSand;
        case Generator::ARoseDesert: return B::PinkSand;
        case Generator::AAshlands: return B::Ash;
        case Generator::ABlight: return B::BlightGrass;
        // NOCHE-MUNDO: biomas frondosos.
        case Generator::AJungle: return B::JungleGrass;
        case Generator::ABlossom: return B::BlossomGrass;
        case Generator::ASwamp: return B::SwampGrass;
        case Generator::ASavanna: return B::SavannaGrass;
        case Generator::AAutumn: return B::LeafLitter;
        default: return B::Grass;
    }
}
BlockId hostileUnder(int biome) {
    switch (biome) {
        case Generator::ADesert:
        case Generator::ABeach: return B::Sand;
        case Generator::ABadlands: return B::RedSand;
        case Generator::ARoseDesert: return B::PinkSand;
        case Generator::AAshlands: return B::Ash;
        case Generator::ABlight: return B::DuskSoil;
        case Generator::ASwamp: return B::Mud;  // NOCHE-MUNDO
        default: return B::Dirt;
    }
}

std::string dimensionLabel(const std::string& dim) {
    if (dim == "abismo") return "the Cinderdeep";
    if (dim == "cielo") return "the Driftreach";
    if (dim == "cristal") return "the Glimmerwood";
    return dim;
}

// --- Planificador de una region ---------------------------------------------------------

// CIUDADES-AMPLIAS: trazado amplio de ciudades y aldeas (calles anchas, plaza grande,
// edificios mayores y separados, mas altos hacia el centro). Cambia la forma de los
// asentamientos (y lo demas de su region), asi que se limita por anillo de regiones
// (Chebyshev desde la 0,0): con kAmpleRing = 1 todas menos la de la capital; subirlo
// deja las cercanas al origen como estaban (ediciones guardadas por coordenada).
// La capital solo cambia con kAmpleCapital (y entonces hay que actualizar la huella
// del plan en VoxelStructureTests.inc).
constexpr int kAmpleRing = 2;  // decidido 07/11: lo cercano al inicio (donde mas se construyo) no cambia
constexpr bool kAmpleCapital = false;
// LADERAS: desnivel maximo (m) del sitio de una aldea amplia (las aldeas se adaptan a
// la ladera con bancales y escaleras). Mas alto = mas aldeas en las colinas (con 22,
// ~+35 % de aldeas que con el 11 de antes, que solo admitia sitios casi llanos).
constexpr int kHillRelief = 22;

struct Planner {
    const Generator& gen;
    std::string dimension;
    int sea;
    std::uint32_t seed;
    int rx, rz;
    int cx0, cz0;  // centro de la region
    RegionPlan& out;
    Rng rng;

    Planner(const Generator& g, std::string dim, std::uint32_t world_seed, int rx_, int rz_, RegionPlan& o)
        : gen(g), dimension(std::move(dim)), sea(g.seaLevel()), seed(world_seed), rx(rx_), rz(rz_), cx0(rx_ * kRegionSize),
          cz0(rz_ * kRegionSize), out(o), rng(hashOf(world_seed, rx_, rz_, 0x5717)) {}

    Generator::AdventureColumn col(int x, int z) const { return gen.adventureColumn(x, z); }
    // CIUDADES-AMPLIAS: esta region usa el trazado amplio.
    bool ampleRegion() const { return std::max(std::abs(rx), std::abs(rz)) >= kAmpleRing && !(rx == 0 && rz == 0); }

    struct SiteStats {
        int min = 999, max = -999, median = 64;
        int water = 0, samples = 0;
        int biome = Generator::AMeadow;
        float temperature = 0.0f;
        int lava = 0;            // muestras en lava (paramo de ceniza)
        float humidity = 0.0f;   // media
    };
    // Entradas de los asentamientos amigos (fin de sus calles de salida): los
    // caminos entre regiones llegan aqui (planRoads).
    std::vector<std::array<float, 2>> hubs;
    SiteStats site(int cx, int cz, int radius, int grid) const {
        SiteStats s;
        std::vector<int> heights;
        std::array<int, Generator::kAdventureBiomeCount> biomes{};
        for (int j = 0; j < grid; ++j) {
            for (int i = 0; i < grid; ++i) {
                const float fx = grid > 1 ? static_cast<float>(i) / static_cast<float>(grid - 1) * 2.0f - 1.0f : 0.0f;
                const float fz = grid > 1 ? static_cast<float>(j) / static_cast<float>(grid - 1) * 2.0f - 1.0f : 0.0f;
                const auto c = col(cx + static_cast<int>(fx * static_cast<float>(radius)), cz + static_cast<int>(fz * static_cast<float>(radius)));
                heights.push_back(c.height);
                s.min = std::min(s.min, c.height);
                s.max = std::max(s.max, c.height);
                if (c.height < sea + 1 || c.river > 0.25f || c.lake) ++s.water;
                ++biomes[static_cast<std::size_t>(c.biome)];
                s.temperature += c.temperature;
                s.humidity += c.humidity;
                if (c.lava > c.height) ++s.lava;
                ++s.samples;
            }
        }
        std::sort(heights.begin(), heights.end());
        s.median = heights[heights.size() / 2];
        s.biome = static_cast<int>(std::max_element(biomes.begin(), biomes.end()) - biomes.begin());
        s.temperature /= static_cast<float>(std::max(1, s.samples));
        s.humidity /= static_cast<float>(std::max(1, s.samples));
        return s;
    }

    static BuildStyle styleFor(int biome) {
        switch (biome) {
            case Generator::ASnow:
            case Generator::AMountains: return BuildStyle::Snow;
            case Generator::ADesert:
            case Generator::ABadlands:
            case Generator::ARoseDesert: return BuildStyle::Desert;
            case Generator::ABeach: return BuildStyle::Beach;
            default: return BuildStyle::Temperate;
        }
    }
    BlockId surfaceFor(BuildStyle style) const {
        switch (style) {
            case BuildStyle::Snow: return B::SnowyGrass;
            case BuildStyle::Desert:
            case BuildStyle::Beach: return B::Sand;
            default: return B::Grass;
        }
    }

    int levelAt(int x, int z, float bonus) {
        const float d = std::sqrt(static_cast<float>(x) * static_cast<float>(x) + static_cast<float>(z) * static_cast<float>(z));
        return std::clamp(1 + static_cast<int>(d / 450.0f + bonus + rng.f() * 1.2f), 1, 12);
    }

    // ¿Cabe un circulo (x, z, r) lejos de las demas estructuras de la region?
    bool clearSpot(int x, int z, float r) const {
        if (std::abs(x - cx0) + r > kHalf - 4 || std::abs(z - cz0) + r > kHalf - 4) return false;
        bool ok = true;
        out.forEach([&](const Structure& s) {
            const float dx = static_cast<float>(x) - s.clear_x, dz = static_cast<float>(z) - s.clear_z;
            if (std::sqrt(dx * dx + dz * dz) < r + s.clear_radius + 6.0f) ok = false;
        });
        return ok;
    }

    Structure& begin(const std::string& type, const std::string& name, int cx, int y, int cz, int level) {
        out.structures.emplace_back();
        Structure& s = out.structures.back();
        const std::size_t index = out.structures.size() - 1;
        s.info.type = type;
        s.info.name = name;
        s.info.center = core::Vec3{static_cast<float>(cx) + 0.5f, static_cast<float>(y), static_cast<float>(cz) + 0.5f};
        s.info.level = level;
        s.info.seed = hashOf(seed, rx, rz, static_cast<int>(index) + 100);
        s.info.id = (static_cast<std::uint64_t>(hashOf(seed, rx, rz, 7)) << 20 ^ (static_cast<std::uint64_t>(index) & 0xFFFFF)) &
                    ((std::uint64_t{1} << 52) - 1);
        s.info.dimension = dimension;
        s.min_x = s.max_x = cx;
        s.min_z = s.max_z = cz;
        s.min_y = s.max_y = y;
        s.clear_x = static_cast<float>(cx);
        s.clear_z = static_cast<float>(cz);
        return s;
    }

    BuildContext context(BuildStyle style, BlockId roof, int salt) {
        BuildContext ctx;
        ctx.seed = hashOf(seed, rx * 977 + salt, rz, static_cast<int>(out.structures.size()));
        ctx.style = style;
        ctx.roof = roof;
        return ctx;
    }

    // Coloca una pieza: (x, z) esquina minima en el mundo.
    void add(Structure& s, BuildContext& ctx, int x, int y, int z, int rot) {
        Piece p;
        p.plan = std::make_shared<const Blueprint>(std::move(ctx.bp));
        p.x = x;
        p.y = y;
        p.z = z;
        p.rot = rot & 3;
        p.min_x = x;
        p.min_z = z;
        p.max_x = x + p.sizeX() - 1;
        p.max_z = z + p.sizeZ() - 1;
        s.min_x = std::min(s.min_x, p.min_x);
        s.max_x = std::max(s.max_x, p.max_x);
        s.min_z = std::min(s.min_z, p.min_z);
        s.max_z = std::max(s.max_z, p.max_z);
        s.min_y = std::min(s.min_y, y);
        s.max_y = std::max(s.max_y, y + p.plan->h - 1);
        for (const LocalPoint& lp : ctx.points) {
            const int lx = static_cast<int>(std::floor(lp.x)), lz = static_cast<int>(std::floor(lp.z));
            int wx = 0, wz = 0;
            toWorld(p, lx, lz, wx, wz);
            StructurePoint sp;
            sp.kind = lp.kind;
            sp.tag = lp.tag;
            sp.position = core::Vec3{static_cast<float>(wx) + 0.5f, static_cast<float>(y) + lp.y, static_cast<float>(wz) + 0.5f};
            const int d = (lp.dir + p.rot) & 3;
            sp.direction = core::Vec3{static_cast<float>(kFront[d][0]), 0.0f, static_cast<float>(kFront[d][1])};
            s.info.points.push_back(std::move(sp));
        }
        ctx.points.clear();
        s.pieces.push_back(std::move(p));
    }

    void addPoint(Structure& s, const std::string& kind, float x, float y, float z, const std::string& tag = {}, int dir = 2) {
        StructurePoint sp;
        sp.kind = kind;
        sp.tag = tag;
        sp.position = core::Vec3{x, y, z};
        sp.direction = core::Vec3{static_cast<float>(kFront[dir & 3][0]), 0.0f, static_cast<float>(kFront[dir & 3][1])};
        s.info.points.push_back(std::move(sp));
    }

    void finish(Structure& s) {
        s.info.min = core::Vec3{static_cast<float>(s.min_x), static_cast<float>(s.min_y), static_cast<float>(s.min_z)};
        s.info.max = core::Vec3{static_cast<float>(s.max_x + 1), static_cast<float>(s.max_y + 1), static_cast<float>(s.max_z + 1)};
        // CIUDADES-GRANDES: caja de las calles (blocksTrees no mira cada calle si el punto cae lejos).
        if (!s.roads.empty()) {
            s.road_x0 = s.road_z0 = 1e30f;
            s.road_x1 = s.road_z1 = -1e30f;
            for (const Road& r : s.roads) {
                s.road_x0 = std::min(s.road_x0, std::min(r.x0, r.x1) - r.half_width);
                s.road_x1 = std::max(s.road_x1, std::max(r.x0, r.x1) + r.half_width);
                s.road_z0 = std::min(s.road_z0, std::min(r.z0, r.z1) - r.half_width);
                s.road_z1 = std::max(s.road_z1, std::max(r.z0, r.z1) + r.half_width);
            }
        }
    }

    // Giro cuya fachada mira mas hacia (fx, fz).
    static int rotFacing(float fx, float fz) {
        int best = 0;
        float best_dot = -9.0f;
        for (int r = 0; r < 4; ++r) {
            const float d = static_cast<float>(kFront[r][0]) * fx + static_cast<float>(kFront[r][1]) * fz;
            if (d > best_dot) {
                best_dot = d;
                best = r;
            }
        }
        return best;
    }

    // --- Ocupacion y calles ---
    struct Lot {
        std::vector<Rect> used;
        const std::vector<Road>* roads = nullptr;
        bool freeRect(const Rect& r, int margin) const {
            for (const Rect& u : used)
                if (u.overlaps(r, margin)) return false;
            return true;
        }
        bool clearOfRoads(const Rect& r, float extra) const {
            for (const Road& road : *roads) {
                const float len = std::hypot(road.x1 - road.x0, road.z1 - road.z0);
                const int steps = std::max(1, static_cast<int>(len));
                for (int i = 0; i <= steps; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(steps);
                    const float px = road.x0 + (road.x1 - road.x0) * t, pz = road.z0 + (road.z1 - road.z0) * t;
                    const float qx = std::clamp(px, static_cast<float>(r.x0), static_cast<float>(r.x1) + 1.0f);
                    const float qz = std::clamp(pz, static_cast<float>(r.z0), static_cast<float>(r.z1) + 1.0f);
                    if (std::hypot(px - qx, pz - qz) < road.half_width + extra) return false;
                }
            }
            return true;
        }
    };

    // Construye una pieza con `build` y la pone junto a la calle: frente a
    // (P + n * dist), mirando hacia -n. Devuelve false si no cabe.
    // CIUDADES-AMPLIAS: `margin` = hueco minimo con lo ya puesto (antes siempre 1)
    // y `setback` = acera entre la calle y la fachada (antes 0).
    template <typename Fn>
    bool placeFacing(Structure& s, Lot& lot, BuildContext& ctx, Fn&& buildFn, int w, int d, float px, float pz, float nx, float nz, float road_half,
                     int y, const std::function<bool(const Rect&)>& inside, int margin = 1, float setback = 0.0f) {
        const int rot = rotFacing(-nx, -nz);
        // (el tamano se sabe antes de construir: no se construye lo que no cabe)
        const int sx = (rot & 1) ? d : w, sz = (rot & 1) ? w : d;
        const float depth = std::abs(nx) * static_cast<float>(sx) + std::abs(nz) * static_cast<float>(sz);
        const float dist = road_half + 0.5f + setback + depth * 0.5f;
        const float ccx = px + nx * dist, ccz = pz + nz * dist;
        const int x0 = static_cast<int>(std::lround(ccx - static_cast<float>(sx) * 0.5f));
        const int z0 = static_cast<int>(std::lround(ccz - static_cast<float>(sz) * 0.5f));
        const Rect r{x0, z0, x0 + sx - 1, z0 + sz - 1};
        if (!inside(r) || !lot.freeRect(r, margin) || !lot.clearOfRoads(r, 0.0f)) {
            ctx.points.clear();
            return false;
        }
        lot.used.push_back(r);
        buildFn(ctx);
        add(s, ctx, x0, y, z0, rot);
        return true;
    }

    // --- LADERAS: suelo de los asentamientos en ladera -------------------------
    // En vez de aplanar todo a una altura, el suelo sigue el terreno: bancales de
    // 3 m con muros de contencion, calles que suben con escalones de 1 bloque (como
    // mucho uno cada 2 m: se suben andando), plataformas para cada casa a la altura
    // de su calle (zocalo de piedra si la ladera baja) y la plaza llana. Todo en
    // enteros (mismo resultado en todas las plataformas).
    struct Hill {
        int x0 = 0, z0 = 0, n = 0;   // ventana n x n
        int base = 64;               // altura de la plaza
        int gstep = 4, gn = 0;       // rejilla gruesa del terreno natural
        std::vector<int> coarse16;   // natural suavizado (x16)
        // Natural suavizado (x16) en (x, z), interpolado de la rejilla gruesa.
        int smooth16(int x, int z) const {
            const int fx = std::clamp(x - x0, 0, n - 1), fz = std::clamp(z - z0, 0, n - 1);
            const int i = fx / gstep, j = fz / gstep, ax = fx % gstep, az = fz % gstep;
            const auto c = [&](int a, int b) { return coarse16[static_cast<std::size_t>(std::min(a, gn - 1) + std::min(b, gn - 1) * gn)]; };
            const int g = gstep;
            return (c(i, j) * (g - ax) * (g - az) + c(i + 1, j) * ax * (g - az) + c(i, j + 1) * (g - ax) * az + c(i + 1, j + 1) * ax * az) / (g * g);
        }
    };
    Hill hillField(int cx, int cz, int E, int gstep = 4) const {
        Hill h;
        h.gstep = gstep;
        h.n = 2 * E + 1;
        h.x0 = cx - E;
        h.z0 = cz - E;
        h.gn = (h.n - 1) / h.gstep + 2;
        std::vector<int> raw(static_cast<std::size_t>(h.gn * h.gn));
        for (int j = 0; j < h.gn; ++j)
            for (int i = 0; i < h.gn; ++i) {
                const auto c = col(h.x0 + i * h.gstep, h.z0 + j * h.gstep);
                raw[static_cast<std::size_t>(i + j * h.gn)] = std::max(c.height, sea + 1) * 16;
            }
        // Dos pasadas de media 3 x 3 (enteros).
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<int> out(raw.size());
            for (int j = 0; j < h.gn; ++j)
                for (int i = 0; i < h.gn; ++i) {
                    int sum = 0, cnt = 0;
                    for (int b = std::max(0, j - 1); b <= std::min(h.gn - 1, j + 1); ++b)
                        for (int a = std::max(0, i - 1); a <= std::min(h.gn - 1, i + 1); ++a) {
                            sum += raw[static_cast<std::size_t>(a + b * h.gn)];
                            ++cnt;
                        }
                    out[static_cast<std::size_t>(i + j * h.gn)] = sum / cnt;
                }
            raw.swap(out);
        }
        h.coarse16 = std::move(raw);
        return h;
    }
    static int roundDiv(int a, int b) { return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b); }
    // Bancales: el terreno suavizado redondeado a escalones de 3 m alrededor de la plaza.
    int terraceAt(const Hill& h, int x, int z) const {
        const int d16 = h.smooth16(x, z) - h.base * 16;
        const int q = std::clamp(roundDiv(d16, 48), -6, 6);
        return std::max(h.base + 3 * q, sea + 1);
    }
    static void groundPut(Structure& s, int x, int z, int height, std::uint8_t kind) {
        const int i = x - s.ground_x0, j = z - s.ground_z0;
        if (i < 0 || j < 0 || i >= s.ground_n || j >= s.ground_n) return;
        const std::size_t k = static_cast<std::size_t>(i + j * s.ground_n);
        s.ground[k] = static_cast<std::int16_t>(height);
        s.ground_kind[k] = kind;
    }
    static int groundOr(const Structure& s, int x, int z, int fallback) {
        int h = fallback;
        s.groundAt(x, z, h);
        return h;
    }
    // Perfil de una calle recta: altura (entera) cada metro desde (x0, z0). Llana
    // hasta `flat` m, luego sigue el terreno suavizado subiendo o bajando como
    // mucho 1 bloque cada 2 m.
    std::vector<int> roadProfile(const Hill& h, const Road& r, int start, float flat) const {
        const float len = std::hypot(r.x1 - r.x0, r.z1 - r.z0);
        const int steps = std::max(1, static_cast<int>(std::ceil(len)));
        std::vector<int> prof(static_cast<std::size_t>(steps + 1));
        int h16 = start * 16;
        for (int t = 0; t <= steps; ++t) {
            const float u = static_cast<float>(t) / static_cast<float>(steps);
            const int x = static_cast<int>(std::floor(r.x0 + (r.x1 - r.x0) * u)), z = static_cast<int>(std::floor(r.z0 + (r.z1 - r.z0) * u));
            if (static_cast<float>(t) > flat) h16 += std::clamp(h.smooth16(x, z) - h16, -8, 8);
            prof[static_cast<std::size_t>(t)] = std::max(roundDiv(h16, 16), sea + 1);
        }
        return prof;
    }
    // Pinta la calle en el suelo: cada celda a menos de half + 0.5 del eje toma la
    // altura del perfil en su punto del eje (no pisa la plaza).
    void groundRoad(Structure& s, const Road& r, const std::vector<int>& prof) const {
        const float vx = r.x1 - r.x0, vz = r.z1 - r.z0, len2 = vx * vx + vz * vz;
        const float reach = r.half_width + 0.5f;
        const int bx0 = static_cast<int>(std::floor(std::min(r.x0, r.x1) - reach)), bx1 = static_cast<int>(std::ceil(std::max(r.x0, r.x1) + reach));
        const int bz0 = static_cast<int>(std::floor(std::min(r.z0, r.z1) - reach)), bz1 = static_cast<int>(std::ceil(std::max(r.z0, r.z1) + reach));
        const float len = std::sqrt(len2);
        for (int z = bz0; z <= bz1; ++z)
            for (int x = bx0; x <= bx1; ++x) {
                const float px = static_cast<float>(x) + 0.5f, pz = static_cast<float>(z) + 0.5f;
                float u = len2 > 0.0f ? ((px - r.x0) * vx + (pz - r.z0) * vz) / len2 : 0.0f;
                u = std::clamp(u, 0.0f, 1.0f);
                if (std::hypot(px - (r.x0 + vx * u), pz - (r.z0 + vz * u)) > reach) continue;
                int h = 0;
                std::uint8_t kind = 0;
                if (!s.groundAt(x, z, h, &kind) || kind == Structure::GPlaza) continue;
                const int t = std::clamp(static_cast<int>(std::lround(u * len)), 0, static_cast<int>(prof.size()) - 1);
                groundPut(s, x, z, prof[static_cast<std::size_t>(t)], Structure::GRoad);
            }
    }
    // Plataforma (casa, granja...): el rectangulo y un borde de 1, sin pisar calles ni plaza.
    static void groundPad(Structure& s, const Rect& r, int height) {
        for (int z = r.z0 - 1; z <= r.z1 + 1; ++z)
            for (int x = r.x0 - 1; x <= r.x1 + 1; ++x) {
                int h = 0;
                std::uint8_t kind = 0;
                if (!s.groundAt(x, z, h, &kind) || kind == Structure::GRoad || kind == Structure::GStep || kind == Structure::GPlaza) continue;
                groundPut(s, x, z, height, Structure::GPad);
            }
    }
    // Une una plataforma (r, altura h) con la calle mas cercana a la que se llegue
    // en linea recta sin cruzar otra casa, por un sendero que sube o baja como mucho
    // 1 cada 2 m (la altura de la plataforma se acerca a la de la calle si hace
    // falta). Devuelve la altura de la plataforma, o kNoPad si no hay camino (no se
    // pone nada).
    static constexpr int kNoPad = -1000000;
    int padWithPath(Structure& s, const Lot& lot, const Rect& r, int h, BlockId path, BlockId path_alt) const {
        const int mx = (r.x0 + r.x1) / 2, mz = (r.z0 + r.z1) / 2;
        std::vector<std::array<int, 4>> cand;  // d2, z, x, altura
        for (int dz = -40; dz <= 40; ++dz)
            for (int dx = -40; dx <= 40; ++dx) {
                int gh = 0;
                std::uint8_t kind = 0;
                if (!s.groundAt(mx + dx, mz + dz, gh, &kind)) continue;
                if (kind != Structure::GRoad && kind != Structure::GStep && kind != Structure::GPlaza) continue;
                cand.push_back({dx * dx + dz * dz, mz + dz, mx + dx, gh});
            }
        std::sort(cand.begin(), cand.end());
        const auto crosses = [&](int sx, int sz, int bx, int bz, int steps) {
            for (int t = 1; t < steps; ++t) {
                const int x = sx + roundDiv((bx - sx) * t, steps), z = sz + roundDiv((bz - sz) * t, steps);
                for (const Rect& u : lot.used)
                    if (!(u.x0 == r.x0 && u.z0 == r.z0 && u.x1 == r.x1 && u.z1 == r.z1) && x >= u.x0 - 1 && x <= u.x1 + 1 && z >= u.z0 - 1 &&
                        z <= u.z1 + 1)
                        return true;
            }
            return false;
        };
        for (std::size_t c = 0; c < cand.size() && c < 80; ++c) {
            const int bx = cand[c][2], bz = cand[c][1], bh = cand[c][3];
            const int sx = std::clamp(bx, r.x0 - 1, r.x1 + 1), sz = std::clamp(bz, r.z0 - 1, r.z1 + 1);
            const float len = std::hypot(static_cast<float>(bx - sx), static_cast<float>(bz - sz));
            const int steps = std::max(1, static_cast<int>(std::ceil(len)));
            if (crosses(sx, sz, bx, bz, steps)) continue;
            h = std::clamp(h, bh - steps / 2, bh + steps / 2);
            groundPad(s, r, h);
            if (len >= 1.0f) {
                const Road rd{static_cast<float>(sx) + 0.5f, static_cast<float>(sz) + 0.5f, static_cast<float>(bx) + 0.5f, static_cast<float>(bz) + 0.5f,
                              0.9f, path, path_alt, 0.25f};
                std::vector<int> prof(static_cast<std::size_t>(steps + 1));
                for (int t = 0; t <= steps; ++t) prof[static_cast<std::size_t>(t)] = h + roundDiv((bh - h) * t, steps);
                groundRoad(s, rd, prof);
                s.roads.push_back(rd);
            }
            return h;
        }
        return kNoPad;
    }
    // Marca los escalones: la celda de calle mas alta de cada contrahuella.
    static int groundSteps(Structure& s) {
        int steps = 0;
        const int n = s.ground_n;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const std::size_t k = static_cast<std::size_t>(i + j * n);
                if (s.ground_kind[k] != Structure::GRoad) continue;
                static constexpr int kN[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : kN) {
                    const int a = i + d[0], b = j + d[1];
                    if (a < 0 || b < 0 || a >= n || b >= n) continue;
                    const std::size_t q = static_cast<std::size_t>(a + b * n);
                    if ((s.ground_kind[q] == Structure::GRoad || s.ground_kind[q] == Structure::GStep) && s.ground[q] < s.ground[k]) {
                        s.ground_kind[k] = Structure::GStep;
                        ++steps;
                        break;
                    }
                }
            }
        return steps;
    }

    // --- Aldea ---------------------------------------------------------------
    bool tryVillage() {
        const bool ample = ampleRegion();
        for (int attempt = 0; attempt < 6; ++attempt) {
            const int R = (ample ? 38 : 32) + rng.range(0, 14);  // CIUDADES-AMPLIAS: aldeas algo mayores
            const int lim = kHalf - R - 30;
            const int cx = cx0 + rng.range(-lim, lim), cz = cz0 + rng.range(-lim, lim);
            if (!clearSpot(cx, cz, static_cast<float>(R + 24))) continue;
            const SiteStats st = site(cx, cz, R, 5);
            // CIUDADES-AMPLIAS / LADERAS: las amplias se adaptan a la ladera (hasta 22 m de desnivel).
            if (st.water > 1 || st.max - st.min > (ample ? kHillRelief : 9) || st.min < sea + 1) continue;
            if (st.biome == Generator::AMountains || st.biome == Generator::AOcean || st.biome == Generator::ARiver) continue;
            if (st.biome == Generator::AAshlands || st.biome == Generator::ABlight) continue;  // nadie vive en el paramo ni en lo marchito
            // Estilo: el del bioma; en el bosque, a veces elfica; en lo muy humedo, palafitos.
            BuildStyle style = styleFor(st.biome);
            const float pick = static_cast<float>(hashOf(seed, cx, cz, 0xE1F) & 0xFFFF) / 65536.0f;
            if (style == BuildStyle::Temperate && st.humidity > 0.25f && pick < 0.7f) style = BuildStyle::Swamp;
            else if (style == BuildStyle::Temperate && st.biome == Generator::AForest && pick < 0.65f) style = BuildStyle::Elven;
            buildVillage(cx, cz, R, std::max(st.median, sea + 2), style, ample);
            return true;
        }
        return false;
    }

    void buildVillage(int cx, int cz, int R, int base, BuildStyle style, bool ample = false) {
        Rng nr(hashOf(seed, rx, rz, 31));
        // LADERAS: en las amplias el suelo sigue el terreno; la plaza, a la altura del centro.
        const bool hill = ample && style != BuildStyle::Swamp;
        Hill hf;
        if (hill) {
            hf = hillField(cx, cz, R + 20);
            base = std::max(roundDiv(hf.smooth16(cx, cz), 16), sea + 2);
            hf.base = base;
        }
        Structure& s = begin("aldea", villageName(nr, style), cx, base + 1, cz, levelAt(cx, cz, 0.0f));
        s.clear_radius = static_cast<float>(R + 22);
        Terraform tf;
        tf.cx = cx;
        tf.cz = cz;
        tf.inner = static_cast<float>(R + 4);
        tf.outer = static_cast<float>(R + 18);
        tf.base = base;
        tf.surface = surfaceFor(style);
        tf.under = style == BuildStyle::Desert || style == BuildStyle::Beach ? B::Sand : B::Dirt;
        tf.decor = style == BuildStyle::Temperate ? 0.08f : (style == BuildStyle::Elven ? 0.1f : 0.025f);
        s.terraforms.push_back(tf);
        static const BlockId kRoofs[] = {B::RoofRed, B::RoofBlue, B::RoofGreen, B::RoofOrange};
        BlockId main_roof = style == BuildStyle::Snow ? (rng.f() < 0.5f ? B::RoofSlate : B::RoofBlue) : rng.pick(kRoofs);
        BlockId alt_roof = style == BuildStyle::Snow ? main_roof : rng.pick(kRoofs);
        if (style == BuildStyle::Elven) {
            main_roof = B::RoofGreen;
            alt_roof = alt_roof == B::RoofRed || alt_roof == B::RoofOrange ? B::RoofGreen : alt_roof;
        }
        const auto roof = [&]() { return rng.f() < 0.7f ? main_roof : alt_roof; };

        // Caminos desde la plaza (pasarelas de tablones en el pantano, musgo en el bosque elfico).
        BlockId path = style == BuildStyle::Desert ? B::Sandstone : B::DirtPath;
        BlockId path_alt = style == BuildStyle::Desert ? B::Sand : (style == BuildStyle::Snow ? B::Snow : B::Gravel);
        if (style == BuildStyle::Swamp) {
            path = B::OakPlanks;
            path_alt = B::DarkPlanks;
        } else if (style == BuildStyle::Elven) {
            path_alt = B::Moss;
        }
        // CIUDADES-AMPLIAS: plaza de 18 m (antes 13) y caminos de 4.4 m (antes 3.2).
        const float plaza_r = ample ? 9.0f : 6.5f, path_half = ample ? 2.2f : 1.6f;
        if (hill) {
            // LADERAS: bancales en toda la ventana y la plaza llana.
            s.ground_x0 = hf.x0;
            s.ground_z0 = hf.z0;
            s.ground_n = hf.n;
            s.ground.assign(static_cast<std::size_t>(hf.n * hf.n), 0);
            s.ground_kind.assign(static_cast<std::size_t>(hf.n * hf.n), Structure::GTerrain);
            for (int j = 0; j < hf.n; ++j)
                for (int i = 0; i < hf.n; ++i) {
                    const int x = hf.x0 + i, z = hf.z0 + j;
                    const int dx = x - cx, dz = z - cz;
                    const int pr2 = static_cast<int>(plaza_r) + 2;
                    if (dx * dx + dz * dz <= pr2 * pr2) groundPut(s, x, z, base, Structure::GPlaza);
                    else groundPut(s, x, z, terraceAt(hf, x, z), Structure::GTerrain);
                }
        }
        // Altura del suelo en (x, z) (la plaza si no hay ladera).
        const auto yAt = [&](int x, int z) { return hill ? groundOr(s, x, z, base) : base; };
        std::vector<std::vector<int>> profiles;  // LADERAS: perfil de cada calle (por metro)
        Road plaza{static_cast<float>(cx), static_cast<float>(cz), static_cast<float>(cx), static_cast<float>(cz), plaza_r, path, path_alt, 0.25f};
        s.roads.push_back(plaza);
        const int n_roads = 3 + (rng.f() < 0.5f ? 1 : 0);
        const float a0 = rng.f() * 2.0f * kPi;
        std::vector<std::array<float, 2>> dirs;
        for (int i = 0; i < n_roads; ++i) {
            const float a = a0 + static_cast<float>(i) * 2.0f * kPi / static_cast<float>(n_roads) + (rng.f() - 0.5f) * 0.5f;
            const float dx = std::cos(a), dz = std::sin(a);
            dirs.push_back({dx, dz});
            s.roads.push_back(Road{static_cast<float>(cx), static_cast<float>(cz), static_cast<float>(cx) + dx * static_cast<float>(R + 14),
                                   static_cast<float>(cz) + dz * static_cast<float>(R + 14), path_half, path, path_alt, 0.25f});
            if (hill) {
                profiles.push_back(roadProfile(hf, s.roads.back(), base, plaza_r + 2.0f));
                groundRoad(s, s.roads.back(), profiles.back());
            }
            hubs.push_back({static_cast<float>(cx) + dx * static_cast<float>(R + 14), static_cast<float>(cz) + dz * static_cast<float>(R + 14)});
        }
        Lot lot;
        lot.roads = &s.roads;
        const int pr = ample ? 9 : 7;
        lot.used.push_back(Rect{cx - pr, cz - pr, cx + pr, cz + pr});
        if (style == BuildStyle::Elven) {
            // Arbol mayor en la plaza (con su punto "plaza").
            BuildContext ctx = context(style, main_roof, 1);
            build::greatTree(ctx);
            add(s, ctx, cx - 7, base, cz - 7, 0);
        } else {
            // Pozo en la plaza (el plano empieza 3 bajo el suelo).
            {
                BuildContext ctx = context(style, main_roof, 1);
                build::well(ctx);
                add(s, ctx, cx - 2, base - 3, cz - 2, 0);
            }
            addPoint(s, "plaza", static_cast<float>(cx) + 0.5f, static_cast<float>(base + 1), static_cast<float>(cz) + 3.5f);
        }
        const auto inside = [&](const Rect& r) {
            const auto in = [&](int x, int z) { return std::hypot(static_cast<float>(x - cx), static_cast<float>(z - cz)) <= static_cast<float>(R + 2); };
            return in(r.x0, r.z0) && in(r.x1, r.z0) && in(r.x0, r.z1) && in(r.x1, r.z1);
        };
        // Lo que hay que poner: especiales primero y luego casas.
        enum Kind { KTavern, KSmithy, KShopGeneral, KShopPotions, KHouse };
        std::vector<Kind> queue = {KTavern};
        if (rng.f() < 0.75f) queue.push_back(KSmithy);
        if (rng.f() < 0.6f) queue.push_back(KShopGeneral);
        if (rng.f() < 0.35f) queue.push_back(KShopPotions);
        static const char* const kTags[] = {"aldeano", "granjero", "lenador", "cazador", "molinero", "pastor", "tejedora"};
        static const char* const kSnowTags[] = {"lenador", "cazador", "aldeano", "pescador", "peletero"};
        static const char* const kDesertTags[] = {"alfarero", "aldeano", "nomada", "tejedora", "aguador"};
        static const char* const kBeachTags[] = {"pescador", "marinero", "aldeano", "redero"};
        static const char* const kElvenTags[] = {"aldeano", "cazador", "herbolario", "tejedora", "arquero", "lenador"};
        static const char* const kSwampTags[] = {"pescador", "aldeano", "herbolario", "cazador", "redero"};
        constexpr int kStilts = 3;  // alto de los pilotes del pantano
        std::size_t next = 0;
        int placed = 0;
        for (int t = ample ? 12 : 10; t < R; t += 5) {
            for (std::size_t ri = 0; ri < dirs.size(); ++ri) {
                for (int side = -1; side <= 1; side += 2) {
                    const float dx = dirs[ri][0], dz = dirs[ri][1];
                    const float px = static_cast<float>(cx) + dx * static_cast<float>(t), pz = static_cast<float>(cz) + dz * static_cast<float>(t);
                    const float nx = -dz * static_cast<float>(side), nz = dx * static_cast<float>(side);
                    const Kind kind = next < queue.size() ? queue[next] : KHouse;
                    BuildContext ctx = context(style, roof(), 10 + placed);
                    // CIUDADES-AMPLIAS: casas algo mayores
                    int hw = ample ? rng.range(8, 11) : rng.range(7, 9), hd = ample ? rng.range(8, 9) : rng.range(7, 8);
                    const int floors = style == BuildStyle::Desert ? rng.range(1, 2) : (rng.f() < 0.45f ? 2 : 1);
                    std::string tag;
                    switch (style) {
                        case BuildStyle::Snow: tag = rng.pick(kSnowTags); break;
                        case BuildStyle::Desert: tag = rng.pick(kDesertTags); break;
                        case BuildStyle::Beach: tag = rng.pick(kBeachTags); break;
                        case BuildStyle::Elven: tag = rng.pick(kElvenTags); break;
                        case BuildStyle::Swamp: tag = rng.pick(kSwampTags); break;
                        default: tag = rng.pick(kTags); break;
                    }
                    const bool stilts = style == BuildStyle::Swamp && kind == KHouse;
                    const auto fn = [&](BuildContext& c) {
                        switch (kind) {
                            case KTavern: build::tavern(c); break;
                            case KSmithy: build::smithy(c, false); break;
                            case KShopGeneral: build::shop(c, "general"); break;
                            case KShopPotions: build::shop(c, "pociones"); break;
                            default:
                                build::house(c, hw, hd, floors, tag);
                                if (stilts) build::raise(c, kStilts);
                                break;
                        }
                    };
                    const int bw = kind == KTavern ? 15 : (kind == KSmithy ? 12 : (kind == KHouse ? hw + 2 : 11));
                    const int bd = (kind == KTavern ? 13 : (kind == KSmithy ? 10 : (kind == KHouse ? hd + 2 : 10))) + (stilts ? kStilts : 0);
                    // LADERAS: la casa a la altura de la calle delante de su puerta.
                    const int floor_y = hill ? profiles[ri][std::min(static_cast<std::size_t>(t), profiles[ri].size() - 1)] : base;
                    const std::size_t before = lot.used.size();
                    bool ok = placeFacing(s, lot, ctx, fn, bw, bd, px, pz, nx, nz, path_half, floor_y, inside, ample ? 2 : 1, ample ? 0.5f : 0.0f);
                    if (!ok && ample && kind == KHouse) {  // CIUDADES-AMPLIAS: si no cabe, una casa estrecha
                        hw = 7;
                        hd = 7;
                        ok = placeFacing(s, lot, ctx, fn, hw + 2, hd + 2 + (stilts ? kStilts : 0), px, pz, nx, nz, path_half, floor_y, inside, 2, 0.5f);
                    }
                    if (ok && hill && lot.used.size() > before) groundPad(s, lot.used.back(), floor_y);
                    if (ok) {
                        ++placed;
                        if (next < queue.size()) ++next;
                    }
                }
            }
        }
        // Farolas a lo largo de los caminos.
        for (std::size_t ri = 0; ri < dirs.size(); ++ri) {
            for (int t = 8; t < R + 6; t += 10) {
                const float side = (t / 10) % 2 ? 1.0f : -1.0f;
                const float dx = dirs[ri][0], dz = dirs[ri][1];
                const float off = ample ? 3.4f : 2.8f;
                const int x = static_cast<int>(std::lround(static_cast<float>(cx) + dx * static_cast<float>(t) - dz * side * off));
                const int z = static_cast<int>(std::lround(static_cast<float>(cz) + dz * static_cast<float>(t) + dx * side * off));
                const Rect r{x, z, x, z};
                // CIUDADES: a 2 bloques de cualquier edificio (antes 0: quedaban delante de las puertas)
                if (!lot.freeRect(r, 2) || !lot.clearOfRoads(r, 0.0f)) continue;
                lot.used.push_back(r);
                BuildContext ctx = context(style, main_roof, 500 + t);
                build::streetLamp(ctx);
                add(s, ctx, x, yAt(x, z), z, 0);
            }
        }
        // Granjas en las afueras (no en el desierto).
        const int farms = style == BuildStyle::Desert ? 0 : rng.range(2, 4);
        for (int i = 0, made = 0; i < 60 && made < farms; ++i) {
            const float a = rng.f() * 2.0f * kPi;
            const float dist = static_cast<float>(R) - 6.0f - rng.f() * 8.0f;
            const int fw = rng.range(9, 13), fd = rng.range(7, 9);
            const int x0 = cx + static_cast<int>(std::cos(a) * dist) - fw / 2, z0 = cz + static_cast<int>(std::sin(a) * dist) - fd / 2;
            const Rect r{x0, z0, x0 + fw - 1, z0 + fd - 1};
            if (!inside(r) || !lot.freeRect(r, 2) || !lot.clearOfRoads(r, 0.5f)) continue;
            lot.used.push_back(r);
            BuildContext ctx = context(style, main_roof, 600 + i);
            build::farm(ctx, fw, fd);
            // LADERAS: el campo en su bancal (plataforma llana).
            // (con un sendero hasta la calle mas cercana: el granjero llega andando)
            const int fy = hill ? padWithPath(s, lot, r, terraceAt(hf, x0 + fw / 2, z0 + fd / 2), path, path_alt) : base;
            if (fy == kNoPad) {
                lot.used.pop_back();
                continue;
            }
            add(s, ctx, x0, fy, z0, 0);
            ++made;
        }
        // Arboles y bancos sueltos.
        for (int i = 0, made = 0; i < 50 && made < 9; ++i) {
            const float a = rng.f() * 2.0f * kPi;
            const float dist = 8.0f + rng.f() * static_cast<float>(R - 6);
            const int x0 = cx + static_cast<int>(std::cos(a) * dist) - 3, z0 = cz + static_cast<int>(std::sin(a) * dist) - 3;
            const Rect r{x0, z0, x0 + 6, z0 + 6};
            if (!inside(r) || !lot.freeRect(r, 0) || !lot.clearOfRoads(r, 0.0f)) continue;
            lot.used.push_back(Rect{x0 + 2, z0 + 2, x0 + 4, z0 + 4});
            BuildContext ctx = context(style, main_roof, 700 + i);
            int ty = yAt(x0 + 3, z0 + 3);  // LADERAS: en el suelo de su sitio
            if (style == BuildStyle::Desert) {
                build::stall(ctx, B::ClothYellow, "general");
                if (hill) ty = padWithPath(s, lot, Rect{x0 + 1, z0 + 1, x0 + 5, z0 + 5}, ty, path, path_alt);
                if (ty == kNoPad) continue;  // sin camino hasta el puesto: no se pone
                add(s, ctx, x0 + 1, ty, z0 + 2, rng.range(0, 3));
            } else if (style == BuildStyle::Swamp && made % 2 == 0) {
                build::pond(ctx, 7, 7);  // charca con juncos
                if (hill) groundPad(s, Rect{x0, z0, x0 + 6, z0 + 6}, ty);
                add(s, ctx, x0, ty, z0, 0);
            } else {
                build::tree(ctx);
                add(s, ctx, x0, ty, z0, 0);
            }
            ++made;
        }
        for (int i = 0; i < 2; ++i) {
            const int x = cx + (i ? 4 : -5), z = cz + (i ? -5 : 4);
            BuildContext ctx = context(style, main_roof, 800 + i);
            build::bench(ctx);
            add(s, ctx, x, base, z, i);
        }
        // Muelle de pescadores si hay mar cerca (el primero que se encuentre), con su camino.
        for (int side = 0; side < 4; ++side) {
            const int dirx = side == 1 ? 1 : (side == 3 ? -1 : 0), dirz = side == 2 ? 1 : (side == 0 ? -1 : 0);
            int water_at = -1;
            for (int t = R + 6; t < R + 50; ++t) {
                const int x = cx + dirx * t, z = cz + dirz * t;
                const int natural = col(x, z).height;
                const float w = terraformWeight(tf, x, z);
                const float ground = static_cast<float>(natural) + static_cast<float>(yAt(x, z) - natural) * w;
                if (ground < static_cast<float>(sea - 1)) {
                    water_at = t;
                    break;
                }
            }
            if (water_at < 0) continue;
            const int start = water_at - 2;
            const int room = kHalf - 4 - start - std::abs(dirx * (cx - cx0) + dirz * (cz - cz0));
            const int length = std::min(18, room);
            if (length < 8) continue;
            BuildContext pc = context(style, main_roof, 900 + side);
            build::pier(pc, length, sea);
            const int py = sea - 8;
            switch (side) {
                case 0: add(s, pc, cx - 2, py, cz - start - length + 1, 2); break;  // hacia -z
                case 1: add(s, pc, cx + start, py, cz - 2, 3); break;                 // hacia +x
                case 2: add(s, pc, cx - 2, py, cz + start, 0); break;                 // hacia +z
                default: add(s, pc, cx - start - length + 1, py, cz - 2, 1); break;   // hacia -x
            }
            s.roads.push_back(Road{static_cast<float>(cx + dirx * (R - 2)), static_cast<float>(cz + dirz * (R - 2)),
                                   static_cast<float>(cx + dirx * start), static_cast<float>(cz + dirz * start), path_half, path, path_alt, 0.25f});
            if (hill) groundRoad(s, s.roads.back(), roadProfile(hf, s.roads.back(), yAt(cx + dirx * (R - 2), cz + dirz * (R - 2)), 0.0f));
            break;
        }
        if (hill) groundSteps(s);
        finish(s);
    }

    // --- Ciudad (y capital) --------------------------------------------------------
    bool tryCity() {
        // Lejos de la capital, a veces una gran ciudad (murallas de ~140 m, mas torres y mercado).
        const bool great = std::max(std::abs(rx), std::abs(rz)) >= 2 && rng.f() < 0.4f;
        const bool ample = ampleRegion();
        for (int attempt = 0; attempt < 6; ++attempt) {
            // CIUDADES-AMPLIAS: murallas de ~155 m (gran ciudad ~180 m) en vez de ~115 / ~135.
            const int H = great ? (ample ? 86 : 64) + rng.range(0, 8) : (ample ? 74 : 54) + rng.range(0, 8);
            const int lim = kHalf - H - 40;
            if (lim < 0) return false;
            const int cx = cx0 + rng.range(-lim, lim), cz = cz0 + rng.range(-lim, lim);
            if (!clearSpot(cx, cz, static_cast<float>(H + 36))) continue;
            const SiteStats st = site(cx, cz, H, 7);
            if (st.water > 2 || st.max - st.min > (ample ? 20 : 16) || st.biome == Generator::AMountains) continue;
            if (st.biome == Generator::AAshlands || st.biome == Generator::ABlight) continue;
            Rng nr(hashOf(seed, rx, rz, 37));
            buildCity(cx, cz, H, std::max(st.median, sea + 3), cityName(nr), false, styleFor(st.biome), ample);
            return true;
        }
        return false;
    }

    void buildCapital() {
        // El mejor sitio cerca del origen (o el menos malo: se aplana lo que haga falta).
        const int H = kAmpleCapital ? 96 : 84;
        int best_x = 0, best_z = 0, best_base = sea + 3;
        float best_score = -1e9f;
        for (int j = -3; j <= 3; ++j) {
            for (int i = -3; i <= 3; ++i) {
                const int x = i * 12, z = j * 12;
                const SiteStats st = site(x, z, H, 7);
                const float land = 1.0f - static_cast<float>(st.water) / static_cast<float>(st.samples);
                const float score = land * 200.0f - static_cast<float>(std::min(st.max - st.min, 60)) * 1.5f -
                                    std::hypot(static_cast<float>(x), static_cast<float>(z)) * 0.25f -
                                    (st.biome == Generator::AMountains ? 60.0f : 0.0f);
                if (score > best_score) {
                    best_score = score;
                    best_x = x;
                    best_z = z;
                    best_base = std::clamp(st.median, sea + 3, sea + 30);
                }
            }
        }
        buildCity(best_x, best_z, H, best_base, "Kettlecrown", true, BuildStyle::City, kAmpleCapital);
    }

    void buildCity(int cx, int cz, int H, int base, const std::string& name, bool capital, BuildStyle biome_style, bool ample = false) {
        Structure& s = begin("ciudad", name, cx, base + 1, cz, capital ? 1 : levelAt(cx, cz, 0.5f));
        s.clear_radius = static_cast<float>(H) * 1.42f + 30.0f;
        const BuildStyle style = BuildStyle::City;
        Terraform tf;
        tf.cx = cx;
        tf.cz = cz;
        tf.inner = static_cast<float>(capital ? H + 8 : H + 16);  // las demas: sitio llano para los campos de fuera
        tf.outer = static_cast<float>(capital ? H + 30 : H + 34);
        tf.square = true;
        tf.base = base;
        tf.surface = biome_style == BuildStyle::Desert ? B::Sand : (biome_style == BuildStyle::Snow ? B::SnowyGrass : B::Grass);
        tf.under = biome_style == BuildStyle::Desert ? B::Sand : B::Dirt;
        tf.decor = 0.05f;
        s.terraforms.push_back(tf);
        static const BlockId kRoofs[] = {B::RoofRed, B::RoofBlue, B::RoofOrange, B::RoofGreen};
        const BlockId main_roof = capital ? B::RoofRed : rng.pick(kRoofs);
        const BlockId alt_roof = capital ? B::RoofBlue : rng.pick(kRoofs);
        const auto roof = [&]() {
            const float r = rng.f();
            return r < 0.55f ? main_roof : (r < 0.85f ? alt_roof : B::RoofOrange);
        };
        const bool big = !capital && H >= (ample ? 86 : 64);  // gran ciudad
        // CIUDADES-AMPLIAS: plaza de 40-48 m (antes 24 / 32), castillo mayor, avenidas de 10 m y calles de 6 m.
        const int P = ample ? (capital ? 24 : (big ? 22 : 20)) : (capital ? 16 : 12);  // radio de la plaza
        const int castle_w = ample ? (capital ? 51 : (big ? 47 : 41)) : (capital ? 45 : (big ? 41 : 35));  // ancho impar: puerta centrada
        const int castle_d = ample ? (capital ? 44 : (big ? 40 : 34)) : (capital ? 40 : (big ? 36 : 30));
        const float main_half = ample ? 5.0f : 3.0f, sec_half = ample ? 3.0f : 2.0f;
        const int castle_z0 = cz - H + 4;
        const int castle_z1 = castle_z0 + castle_d - 1;
        const float fcx = static_cast<float>(cx), fcz = static_cast<float>(cz);
        // Calles: principales (empedradas) en cruz, anillo junto a la muralla y secundarias.
        const auto street = [&](float x0, float z0, float x1, float z1, float half, BlockId surf, BlockId alt) {
            s.roads.push_back(Road{x0, z0, x1, z1, half, surf, alt, 0.12f});
        };
        street(fcx, fcz, fcx, fcz, static_cast<float>(P), B::Paving, B::StoneBricks);  // plaza redonda
        street(fcx, static_cast<float>(castle_z1 + 1), fcx, fcz + static_cast<float>(H + 14), main_half, B::Paving, B::Cobblestone);
        street(fcx - static_cast<float>(H + 14), fcz, fcx + static_cast<float>(H + 14), fcz, main_half, B::Paving, B::Cobblestone);
        const float ring = static_cast<float>(H - (ample ? 9 : 8));
        // CIUDADES-AMPLIAS: las calles secundarias a medio camino entre la avenida y la ronda
        // (manzanas de ~27 m: dos filas de casas espalda con espalda).
        const float sec = ample ? (ring + main_half) * 0.5f : static_cast<float>(H) * 0.5f;
        const float front = static_cast<float>(castle_z1 + (ample ? 5 : 3));  // calle delante del castillo
        street(fcx - ring, fcz + ring, fcx + ring, fcz + ring, sec_half, B::Cobblestone, B::Gravel);
        street(fcx - ring, fcz - ring, fcx - ring, fcz + ring, sec_half, B::Cobblestone, B::Gravel);
        street(fcx + ring, fcz - ring, fcx + ring, fcz + ring, sec_half, B::Cobblestone, B::Gravel);
        street(fcx - ring, front, fcx + ring, front, sec_half, B::Cobblestone, B::Gravel);
        street(fcx - sec, front, fcx - sec, fcz + ring, sec_half, B::Cobblestone, B::Gravel);
        street(fcx + sec, front, fcx + sec, fcz + ring, sec_half, B::Cobblestone, B::Gravel);
        street(fcx - ring, fcz + sec, fcx + ring, fcz + sec, sec_half, B::Cobblestone, B::Gravel);
        if (fcz - sec > static_cast<float>(castle_z1 + 6) + (ample ? 4.0f : 0.0f))
            street(fcx - ring, fcz - sec, fcx + ring, fcz - sec, sec_half, B::Cobblestone, B::Gravel);

        Lot lot;
        lot.roads = &s.roads;
        // --- Murallas, torres y puertas ---
        const int wall_h = capital ? 10 : 9;
        const int T = 4;
        {
            // Lados: 0 norte, 1 este, 2 sur, 3 oeste. Puerta en E, S y O.
            for (int side = 0; side < 4; ++side) {
                const bool gate = side != 0;
                const int start = -H + 6, end = H - 6;  // tramo entre torres (relativo)
                std::vector<std::array<int, 2>> spans;
                if (gate) {
                    spans.push_back({start, -7});
                    spans.push_back({7, end});
                } else {
                    spans.push_back({start, end});
                }
                for (const auto& sp : spans) {
                    const int len = sp[1] - sp[0] + 1;
                    if (len <= 0) continue;
                    BuildContext ctx = context(style, main_roof, 1000 + side * 7 + sp[0]);
                    build::wallSegment(ctx, len, wall_h);
                    if (!capital) build::wallTorches(ctx, 6);  // antorchas en el adarve (la capital, tal cual)
                    switch (side) {
                        case 0: add(s, ctx, cx + sp[0], base, cz - H, 0); break;
                        case 1: add(s, ctx, cx + H - T, base, cz + sp[0], 1); break;
                        case 2: add(s, ctx, cx + sp[0], base, cz + H - T, 2); break;
                        default: add(s, ctx, cx - H, base, cz + sp[0], 3); break;
                    }
                }
                if (gate) {
                    BuildContext ctx = context(style, main_roof, 1100 + side);
                    build::gatehouse(ctx, 13, wall_h + 3, T);
                    switch (side) {
                        case 1: add(s, ctx, cx + H - T - 2, base, cz - 6, 1); break;
                        case 2: add(s, ctx, cx - 6, base, cz + H - T - 2, 2); break;
                        default: add(s, ctx, cx - H - 2, base, cz - 6, 3); break;
                    }
                }
            }
            // Torres en las esquinas (y a mitad de tramo en la capital).
            // CIUDADES-AMPLIAS: torres mas anchas y altas (mejor silueta desde fuera).
            const int ts = ample ? 11 : 9;
            std::vector<std::array<int, 3>> towers = {{-H + 1, -H + 1, 2}, {H - 2, -H + 1, 2}, {-H + 1, H - 2, 0}, {H - 2, H - 2, 0}};
            if (capital || big) {
                const int m = H / 2;
                towers.push_back({-m, H - 2, 0});
                towers.push_back({m, H - 2, 0});
                towers.push_back({H - 2, -m, 3});
                towers.push_back({H - 2, m, 3});
                towers.push_back({-H + 1, -m, 1});
                towers.push_back({-H + 1, m, 1});
                towers.push_back({-m, -H + 1, 2});
                towers.push_back({m, -H + 1, 2});
            }
            int ti = 0;
            for (const auto& t : towers) {
                BuildContext ctx = context(style, alt_roof, 1200 + ti++);
                build::tower(ctx, (t[0] == -H + 1 || t[0] == H - 2) && (t[1] == -H + 1 || t[1] == H - 2) ? ts : (ample ? 9 : 7),
                             wall_h + (ample ? 10 : 6));
                const int size = ctx.bp.w;
                add(s, ctx, cx + t[0] - size / 2, base, cz + t[1] - size / 2, t[2]);
            }
            lot.used.push_back(Rect{cx - H - 6, cz - H - 6, cx + H + 6, cz - H + 4});
            lot.used.push_back(Rect{cx - H - 6, cz + H - 4, cx + H + 6, cz + H + 6});
            lot.used.push_back(Rect{cx - H - 6, cz - H - 6, cx - H + 4, cz + H + 6});
            lot.used.push_back(Rect{cx + H - 4, cz - H - 6, cx + H + 6, cz + H + 6});
        }
        // --- Castillo (al norte, mirando a la plaza) ---
        {
            BuildContext ctx = context(style, alt_roof, 1300);
            build::castle(ctx, castle_w, castle_d);
            const int x0 = cx - castle_w / 2;
            add(s, ctx, x0, base - 6, castle_z0, 2);
            lot.used.push_back(Rect{x0 - 2, castle_z0, x0 + castle_w + 1, castle_z1 + 1});
        }
        // --- Plaza: fuente, puestos del mercado, farolas ---
        {
            const int fs = ample ? (capital ? 13 : 11) : (capital ? 11 : 9);
            BuildContext ctx = context(style, main_roof, 1400);
            build::fountain(ctx, fs);
            add(s, ctx, cx - fs / 2, base - 1, cz - fs / 2, 0);
            lot.used.push_back(Rect{cx - P, cz - P, cx + P, cz + P});
            addPoint(s, "plaza", fcx + 0.5f, static_cast<float>(base + 1), fcz + static_cast<float>(fs / 2 + 2) + 0.5f);
            if (capital) addPoint(s, "inicio", fcx + 0.5f, static_cast<float>(base + 1), fcz + static_cast<float>(fs / 2 + 4) + 0.5f, "", 0);
            static const char* const kShops[] = {"armas", "armaduras", "pociones", "monturas", "pociones", "armas"};
            static const BlockId kCloth[] = {B::ClothRed, B::ClothBlue, B::ClothYellow, B::ClothRed, B::ClothBlue, B::ClothYellow};
            const int stalls = ample ? (capital ? 14 : (big ? 12 : 8)) : (capital ? 12 : (big ? 10 : 6));
            int made = 0;
            for (int i = 0; i < 24 && made < stalls; ++i) {
                const float a = (static_cast<float>(i) + 0.5f) * 2.0f * kPi / 24.0f;
                const float dx = std::cos(a), dz = std::sin(a);
                if (std::abs(dx) > 0.92f || std::abs(dz) > 0.92f) continue;  // no en las calles
                const float rr = static_cast<float>(P) - (ample ? 6.0f : 3.5f);  // CIUDADES-AMPLIAS: corro libre junto a las fachadas
                BuildContext sc = context(style, main_roof, 1500 + i);
                build::stall(sc, kCloth[made % 6], kShops[made % 6]);
                const int rot = rotFacing(-dx, -dz);
                const int sx = (rot & 1) ? sc.bp.d : sc.bp.w, sz = (rot & 1) ? sc.bp.w : sc.bp.d;
                const int x0 = static_cast<int>(std::lround(fcx + dx * rr - static_cast<float>(sx) * 0.5f));
                const int z0 = static_cast<int>(std::lround(fcz + dz * rr - static_cast<float>(sz) * 0.5f));
                add(s, sc, x0, base, z0, rot);
                ++made;
            }
            for (int i = 0; i < 8; ++i) {
                const float a = (static_cast<float>(i) + 0.5f) * kPi / 4.0f;
                const int x = cx + static_cast<int>(std::lround(std::cos(a) * static_cast<float>(P + 1)));
                const int z = cz + static_cast<int>(std::lround(std::sin(a) * static_cast<float>(P + 1)));
                BuildContext lc = context(style, main_roof, 1600 + i);
                if (capital) build::lampPost(lc);  // la capital, tal cual
                else build::streetLamp(lc);
                add(s, lc, x, base, z, 0);
                lot.used.push_back(Rect{x, z, x, z});
            }
            for (int i = 0; i < 4; ++i) {
                BuildContext bc = context(style, main_roof, 1650 + i);
                build::bench(bc);
                const float a = static_cast<float>(i) * kPi * 0.5f + kPi * 0.25f;
                const float br = static_cast<float>(fs / 2 + (ample ? 5 : 4));
                add(s, bc, cx + static_cast<int>(std::lround(std::cos(a) * br)), base, cz + static_cast<int>(std::lround(std::sin(a) * br)), i);
            }
        }
        // --- Edificios a lo largo de las calles ---
        const int lim = H - 6;
        const auto inside = [&](const Rect& r) {
            return r.x0 >= cx - lim && r.x1 <= cx + lim && r.z0 >= cz - lim && r.z1 <= cz + lim;
        };
        enum Kind { KTavern, KSmithy, KTemple, KPotions, KArmor, KGeneral, KStable, KHouse, KGarden };
        std::vector<Kind> queue = {KTavern, KTemple, KSmithy, KPotions, KArmor, KGeneral, KStable};
        if (ample && (capital || big)) {  // CIUDADES-AMPLIAS: mas sitio, mas servicios
            queue.push_back(KTavern);
            queue.push_back(KSmithy);
        }
        if (capital) {
            queue.push_back(KTavern);
            queue.push_back(KSmithy);
            queue.push_back(KGeneral);
            queue.push_back(KPotions);
        }
        static const char* const kTags[] = {"ciudadano", "artesano", "comerciante", "noble", "aldeano", "guardia", "panadero",
                                            "sastre", "carpintero", "mensajero", "erudito", "nino"};
        std::size_t next = 0;
        int placed = 0;
        // Recorre las calles (principales primero, del centro hacia fuera).
        struct Walk {
            float x0, z0, x1, z1, half;
        };
        std::vector<Walk> walks;
        for (std::size_t i = 1; i < s.roads.size(); ++i) {
            const Road& r = s.roads[i];
            walks.push_back({r.x0, r.z0, r.x1, r.z1, r.half_width});
        }
        // Los brazos de la cruz desde la plaza hacia fuera, intercalados.
        std::vector<Walk> arms = {{fcx, fcz + static_cast<float>(P), fcx, fcz + static_cast<float>(H), main_half},
                                  {fcx + static_cast<float>(P), fcz, fcx + static_cast<float>(H), fcz, main_half},
                                  {fcx - static_cast<float>(P), fcz, fcx - static_cast<float>(H), fcz, main_half},
                                  {fcx, fcz - static_cast<float>(P), fcx, static_cast<float>(castle_z1 + 2), main_half}};
        const auto tryAlong = [&](const Walk& wk, float t, int side) {
            const float len = std::hypot(wk.x1 - wk.x0, wk.z1 - wk.z0);
            if (t > len) return;
            const float dx = (wk.x1 - wk.x0) / len, dz = (wk.z1 - wk.z0) / len;
            const float px = wk.x0 + dx * t, pz = wk.z0 + dz * t;
            const float nx = -dz * static_cast<float>(side), nz = dx * static_cast<float>(side);
            Kind kind = next < queue.size() ? queue[next] : KHouse;
            BuildContext ctx = context(style, roof(), 2000 + placed * 3 + side);
            int hw = rng.range(7, 10), hd = rng.range(7, 9);
            int floors = capital ? rng.range(2, 3) : rng.range(1, 3);
            const std::string tag = rng.pick(kTags);
            if (ample && kind == KHouse) {
                // CIUDADES-AMPLIAS: casas mayores; mas pisos cerca de la plaza y menos junto a la muralla
                // (la silueta sube hacia el centro); a veces una casona o un jardin con su arbol.
                const float zone = std::max(std::abs(px - fcx), std::abs(pz - fcz)) / static_cast<float>(H);
                const float pick = rng.f();
                hw = rng.range(8, 12);
                hd = rng.range(8, 10);
                floors = zone < 0.5f ? rng.range(3, 4) : (zone < 0.8f ? rng.range(2, 3) : 2);
                if (pick < 0.08f) {
                    kind = KGarden;
                    hw = rng.range(9, 11);
                    hd = rng.range(8, 10);
                } else if (pick < 0.25f) {
                    hw = rng.range(13, 15);  // casona
                    hd = rng.range(11, 12);
                    floors = std::max(floors, 3);
                }
            }
            const auto fn = [&](BuildContext& c) {
                switch (kind) {
                    case KGarden: build::garden(c, hw + 2, hd + 2); break;
                    case KTavern: build::tavern(c); break;
                    case KSmithy: build::smithy(c, true); break;
                    case KTemple: c.roof = B::RoofBlue; build::temple(c); break;
                    case KPotions: build::shop(c, "pociones"); break;
                    case KArmor: build::shop(c, "armaduras"); break;
                    case KGeneral: build::shop(c, "general"); break;
                    case KStable: build::stable(c); break;
                    default: build::house(c, hw, hd, floors, tag); break;
                }
            };
            int bw = hw + 2, bd = hd + 2;
            switch (kind) {
                case KTavern: bw = 15; bd = 13; break;
                case KSmithy: bw = 12; bd = 10; break;
                case KTemple: bw = 13; bd = 21; break;
                case KPotions: case KArmor: case KGeneral: bw = 11; bd = 10; break;
                case KStable: bw = 13; bd = 11; break;
                default: break;
            }
            bool ok = placeFacing(s, lot, ctx, fn, bw, bd, px, pz, nx, nz, wk.half, base, inside, ample ? 2 : 1, ample ? 1.0f : 0.0f);
            if (!ok && ample && (kind == KHouse || kind == KGarden)) {
                // CIUDADES-AMPLIAS: si no cabe, una casa estrecha (llena los huecos de la fila).
                kind = KHouse;
                hw = rng.range(7, 8);
                hd = rng.range(8, 9);
                ok = placeFacing(s, lot, ctx, fn, hw + 2, hd + 2, px, pz, nx, nz, wk.half, base, inside, 2, 1.0f);
            }
            if (ok) {
                ++placed;
                if (next < queue.size()) ++next;
            }
        };
        for (float t = 1.0f; t < static_cast<float>(H); t += 4.0f) {
            for (const Walk& a : arms) {
                tryAlong(a, t, 1);
                tryAlong(a, t, -1);
            }
        }
        for (const Walk& wk : walks) {
            const float len = std::hypot(wk.x1 - wk.x0, wk.z1 - wk.z0);
            for (float t = 2.0f; t < len; t += 3.0f) {
                tryAlong(wk, t, 1);
                tryAlong(wk, t, -1);
            }
        }
        // Farolas en las calles principales y arboles/bancos en los huecos.
        for (const Walk& a : arms) {
            const float len = std::hypot(a.x1 - a.x0, a.z1 - a.z0);
            const float dx = (a.x1 - a.x0) / len, dz = (a.z1 - a.z0) / len;
            for (float t = 6.0f; t < len; t += 10.0f) {
                for (int side = -1; side <= 1; side += 2) {
                    const float off = ample ? main_half - 0.5f : 3.6f;  // CIUDADES-AMPLIAS: en el borde de la avenida
                    const int x = static_cast<int>(std::lround(a.x0 + dx * t - dz * static_cast<float>(side) * off));
                    const int z = static_cast<int>(std::lround(a.z0 + dz * t + dx * static_cast<float>(side) * off));
                    const Rect r{x, z, x, z};
                    if (!lot.freeRect(r, 2)) continue;  // CIUDADES: no delante de las puertas
                    lot.used.push_back(r);
                    BuildContext lc = context(style, main_roof, 3000 + static_cast<int>(t) * 2 + side);
                    if (capital) build::lampPost(lc);
                    else build::streetLamp(lc);
                    add(s, lc, x, base, z, 0);
                }
            }
        }
        // CIUDADES-AMPLIAS: patios interiores. Lo que queda libre detras de las filas de casas
        // (manzanas hondas) se llena de jardines con su arbol, barriendo en un orden fijo.
        if (ample) {
            int k = 0;
            for (int z = cz - lim; z <= cz + lim - 9; z += 3) {
                for (int x = cx - lim; x <= cx + lim - 9; x += 3) {
                    for (int gw = 13; gw >= 9; gw -= 2) {
                        const Rect r{x, z, x + gw - 1, z + gw - 1};
                        if (!inside(r) || !lot.freeRect(r, 1) || !lot.clearOfRoads(r, 0.5f)) continue;
                        lot.used.push_back(r);
                        BuildContext gc = context(style, main_roof, 3300 + k++);
                        build::garden(gc, gw, gw);
                        add(s, gc, x, base, z, static_cast<int>(hashOf(seed, x, z, 0x6A2E) & 3u));
                        break;
                    }
                }
            }
        }
        for (int i = 0, made = 0; i < (ample ? 260 : 160) && made < (ample ? (capital ? 40 : 24) : (capital ? 26 : 12)); ++i) {
            const int x0 = cx + rng.range(-lim, lim - 7), z0 = cz + rng.range(-lim, lim - 7);
            const Rect r{x0, z0, x0 + 6, z0 + 6};
            if (!inside(r) || !lot.freeRect(r, 0) || !lot.clearOfRoads(r, 0.0f)) continue;
            lot.used.push_back(Rect{x0 + 1, z0 + 1, x0 + 5, z0 + 5});
            BuildContext tc = context(style, main_roof, 3500 + i);
            build::tree(tc);
            add(s, tc, x0, base, z0, 0);
            ++made;
        }
        // Guardias de ronda en los cruces.
        addPoint(s, "npc", fcx + static_cast<float>(P) + 1.5f, static_cast<float>(base + 1), fcz + 3.5f, "guardia", 1);
        addPoint(s, "npc", fcx - static_cast<float>(P) - 1.5f, static_cast<float>(base + 1), fcz - 3.5f, "guardia", 3);
        addPoint(s, "npc", fcx + 3.5f, static_cast<float>(base + 1), fcz + static_cast<float>(P) + 1.5f, "guardia", 2);
        // --- Puerto: muelle si hay mar cerca de una puerta ---
        for (int side = 1; side < 4; ++side) {
            const int dirx = side == 1 ? 1 : (side == 3 ? -1 : 0), dirz = side == 2 ? 1 : 0;
            int water_at = -1;
            for (int t = H + 6; t < H + 80; t += 1) {
                const int x = cx + dirx * t, z = cz + dirz * t;
                const int natural = col(x, z).height;
                const float w = terraformWeight(tf, x, z);
                const float ground = static_cast<float>(natural) + static_cast<float>(base - natural) * w;  // tras aplanar
                if (ground < static_cast<float>(sea - 1)) {
                    water_at = t;
                    break;
                }
            }
            if (water_at < 0) continue;
            const int start = water_at - 2;
            const int room = kHalf - 4 - start - std::abs(dirx * (cx - cx0) + dirz * (cz - cz0));
            const int length = std::min(capital ? 34 : 24, room);
            if (length < 10) continue;
            BuildContext pc = context(style, main_roof, 3800 + side);
            build::pier(pc, length, sea);
            const int py = sea - 8;
            switch (side) {
                case 1: add(s, pc, cx + start, py, cz - 2, 3); break;                // hacia +x
                case 2: add(s, pc, cx - 2, py, cz + start, 0); break;                // hacia +z
                default: add(s, pc, cx - start - length + 1, py, cz - 2, 1); break;  // hacia -x
            }
            street(fcx + static_cast<float>(dirx * (H + 2)), fcz + static_cast<float>(dirz * (H + 2)), fcx + static_cast<float>(dirx * start),
                   fcz + static_cast<float>(dirz * start), 2.5f, B::Paving, B::Cobblestone);
        }
        // Campos de cultivo junto a la muralla, por fuera (no en la capital).
        if (!capital) outerFarms(s, lot, cx, cz, H, base, biome_style, main_roof);
        // Entradas para los caminos entre regiones: el final de las calles de las puertas.
        hubs.push_back({fcx + static_cast<float>(H + 14), fcz});
        hubs.push_back({fcx, fcz + static_cast<float>(H + 14)});
        hubs.push_back({fcx - static_cast<float>(H + 14), fcz});
        finish(s);
    }

    // Campos de cultivo por fuera de una muralla cuadrada de semilado H (dos por lado, lejos de las puertas).
    void outerFarms(Structure& s, Lot& lot, int cx, int cz, int H, int base, BuildStyle style, BlockId roof) {
        for (int side = 0; side < 4; ++side) {
            for (int k = -1; k <= 1; k += 2) {
                const int a = rng.range(11, 14), b = rng.range(7, 9);  // a lo largo y hacia fuera
                const int along = (side % 2 == 0 ? cx : cz) + k * H / 2;
                int x0 = 0, z0 = 0, w = a, d = b;
                switch (side) {
                    case 0: x0 = along - a / 2; z0 = cz - H - 8 - b + 1; break;
                    case 2: x0 = along - a / 2; z0 = cz + H + 8; break;
                    case 1: w = b; d = a; x0 = cx + H + 8; z0 = along - a / 2; break;
                    default: w = b; d = a; x0 = cx - H - 8 - b + 1; z0 = along - a / 2; break;
                }
                const Rect r{x0, z0, x0 + w - 1, z0 + d - 1};
                if (std::abs(r.x0 - cx0) > kHalf - 4 || std::abs(r.x1 - cx0) > kHalf - 4 || std::abs(r.z0 - cz0) > kHalf - 4 ||
                    std::abs(r.z1 - cz0) > kHalf - 4)
                    continue;
                if (!lot.freeRect(r, 1) || !lot.clearOfRoads(r, 1.0f)) continue;
                lot.used.push_back(r);
                BuildContext fc = context(style, roof, 3900 + side * 2 + (k > 0 ? 1 : 0));
                build::farm(fc, w, d);
                add(s, fc, x0, base, z0, 0);
            }
        }
    }

    // Murallas cuadradas de semilado H (como las de la ciudad): tramos, puertas en
    // los lados de `gates` (bit por lado: 0 norte, 1 este, 2 sur, 3 oeste) y torres.
    void squareWalls(Structure& s, Lot& lot, int cx, int cz, int H, int base, int wall_h, unsigned gates, bool mid_towers, BlockId roof,
                     BlockId tower_roof) {
        const BuildStyle style = BuildStyle::City;
        const int T = 4;
        for (int side = 0; side < 4; ++side) {
            const bool gate = (gates >> side) & 1u;
            const int start = -H + 6, end = H - 6;
            std::vector<std::array<int, 2>> spans;
            if (gate) {
                spans.push_back({start, -7});
                spans.push_back({7, end});
            } else {
                spans.push_back({start, end});
            }
            for (const auto& sp : spans) {
                const int len = sp[1] - sp[0] + 1;
                if (len <= 0) continue;
                BuildContext ctx = context(style, roof, 1000 + side * 7 + sp[0]);
                build::wallSegment(ctx, len, wall_h);
                build::wallTorches(ctx, 6);
                switch (side) {
                    case 0: add(s, ctx, cx + sp[0], base, cz - H, 0); break;
                    case 1: add(s, ctx, cx + H - T, base, cz + sp[0], 1); break;
                    case 2: add(s, ctx, cx + sp[0], base, cz + H - T, 2); break;
                    default: add(s, ctx, cx - H, base, cz + sp[0], 3); break;
                }
            }
            if (gate) {
                BuildContext ctx = context(style, roof, 1100 + side);
                build::gatehouse(ctx, 13, wall_h + 3, T);
                switch (side) {
                    case 0: add(s, ctx, cx - 6, base, cz - H - 2, 0); break;
                    case 1: add(s, ctx, cx + H - T - 2, base, cz - 6, 1); break;
                    case 2: add(s, ctx, cx - 6, base, cz + H - T - 2, 2); break;
                    default: add(s, ctx, cx - H - 2, base, cz - 6, 3); break;
                }
            }
        }
        std::vector<std::array<int, 3>> towers = {{-H + 1, -H + 1, 2}, {H - 2, -H + 1, 2}, {-H + 1, H - 2, 0}, {H - 2, H - 2, 0}};
        if (mid_towers) {
            const int m = H / 2;
            towers.push_back({-m, H - 2, 0});
            towers.push_back({m, H - 2, 0});
            towers.push_back({H - 2, -m, 3});
            towers.push_back({H - 2, m, 3});
            towers.push_back({-H + 1, -m, 1});
            towers.push_back({-H + 1, m, 1});
            towers.push_back({-m, -H + 1, 2});
            towers.push_back({m, -H + 1, 2});
        }
        int ti = 0;
        for (const auto& t : towers) {
            BuildContext ctx = context(style, tower_roof, 1200 + ti++);
            const bool corner = (t[0] == -H + 1 || t[0] == H - 2) && (t[1] == -H + 1 || t[1] == H - 2);
            build::tower(ctx, corner ? 9 : 7, wall_h + 6);
            const int size = ctx.bp.w;
            add(s, ctx, cx + t[0] - size / 2, base, cz + t[1] - size / 2, t[2]);
        }
        lot.used.push_back(Rect{cx - H - 6, cz - H - 6, cx + H + 6, cz - H + 4});
        lot.used.push_back(Rect{cx - H - 6, cz + H - 4, cx + H + 6, cz + H + 6});
        lot.used.push_back(Rect{cx - H - 6, cz - H - 6, cx - H + 4, cz + H + 6});
        lot.used.push_back(Rect{cx + H - 4, cz - H - 6, cx + H + 6, cz + H + 6});
    }

    // --- Castillo real (amigo: tipo "ciudad", el juego lo puebla como una ciudad) ---------
    // Gran castillo (salon del trono, torres, biblioteca, calabozos) al fondo de un
    // patio de armas amurallado: puerta al sur, calle mayor, plaza con fuente y
    // mercado, posada, herreria, templo, establo y casas; campos por fuera.
    bool tryRoyalCastle() {
        for (int attempt = 0; attempt < 6; ++attempt) {
            const int H = 44 + rng.range(0, 4);
            const int lim = kHalf - H - 40;
            const int cx = cx0 + rng.range(-lim, lim), cz = cz0 + rng.range(-lim, lim);
            if (!clearSpot(cx, cz, static_cast<float>(H + 36))) continue;
            const SiteStats st = site(cx, cz, H, 7);
            if (st.water > 2 || st.max - st.min > 18 || st.biome == Generator::AMountains || st.min < sea + 1) continue;
            if (st.biome == Generator::AAshlands || st.biome == Generator::ABlight) continue;
            buildRoyalCastle(cx, cz, H, std::max(st.median, sea + 3), styleFor(st.biome));
            return true;
        }
        return false;
    }

    void buildRoyalCastle(int cx, int cz, int H, int base, BuildStyle biome_style) {
        Rng nr(hashOf(seed, rx, rz, 53));
        Structure& s = begin("ciudad", castleName(nr), cx, base + 1, cz, levelAt(cx, cz, 0.5f));
        s.clear_radius = static_cast<float>(H) * 1.42f + 30.0f;
        const BuildStyle style = BuildStyle::City;
        Terraform tf;
        tf.cx = cx;
        tf.cz = cz;
        tf.inner = static_cast<float>(H + 16);
        tf.outer = static_cast<float>(H + 34);
        tf.square = true;
        tf.base = base;
        tf.surface = biome_style == BuildStyle::Desert ? B::Sand : (biome_style == BuildStyle::Snow ? B::SnowyGrass : B::Grass);
        tf.under = biome_style == BuildStyle::Desert ? B::Sand : B::Dirt;
        tf.decor = 0.05f;
        s.terraforms.push_back(tf);
        static const BlockId kRoofs[] = {B::RoofBlue, B::RoofRed, B::RoofSlate, B::RoofPurple, B::RoofGreen};
        const BlockId main_roof = rng.pick(kRoofs);
        const BlockId alt_roof = rng.pick(kRoofs);
        const auto roof = [&]() { return rng.f() < 0.6f ? main_roof : alt_roof; };
        const float fcx = static_cast<float>(cx), fcz = static_cast<float>(cz);
        const int castle_w = 51, castle_d = 44;
        const int castle_z0 = cz - H + 4, castle_z1 = castle_z0 + castle_d - 1;
        const int zc = (castle_z1 + cz + H) / 2;  // calle transversal y plaza
        const auto street = [&](float x0, float z0, float x1, float z1, float half, BlockId surf, BlockId alt) {
            s.roads.push_back(Road{x0, z0, x1, z1, half, surf, alt, 0.12f});
        };
        const float ring = static_cast<float>(H - 8);
        street(fcx, static_cast<float>(zc), fcx, static_cast<float>(zc), 7.0f, B::Paving, B::StoneBricks);  // plaza
        street(fcx, static_cast<float>(castle_z1 + 1), fcx, fcz + static_cast<float>(H + 14), 3.0f, B::Paving, B::Cobblestone);
        street(fcx - ring, static_cast<float>(zc), fcx + ring, static_cast<float>(zc), 2.0f, B::Cobblestone, B::Gravel);
        street(fcx - ring, fcz + ring, fcx + ring, fcz + ring, 2.0f, B::Cobblestone, B::Gravel);
        Lot lot;
        lot.roads = &s.roads;
        squareWalls(s, lot, cx, cz, H, base, 10, 1u << 2, true, main_roof, alt_roof);
        // El castillo, al fondo y mirando a la puerta.
        {
            BuildContext ctx = context(style, alt_roof, 1300);
            build::castle(ctx, castle_w, castle_d);
            const int x0 = cx - castle_w / 2;
            add(s, ctx, x0, base - 6, castle_z0, 2);
            lot.used.push_back(Rect{x0 - 2, castle_z0, x0 + castle_w + 1, castle_z1 + 1});
        }
        // Plaza: fuente, puestos y farolas.
        {
            BuildContext ctx = context(style, main_roof, 1400);
            build::fountain(ctx, 7);
            add(s, ctx, cx - 3, base - 1, zc - 3, 0);
            lot.used.push_back(Rect{cx - 7, zc - 7, cx + 7, zc + 7});
            addPoint(s, "plaza", fcx + 0.5f, static_cast<float>(base + 1), static_cast<float>(zc + 5) + 0.5f);
            static const char* const kShops[] = {"armas", "pociones", "armaduras", "monturas"};
            static const BlockId kCloth[] = {B::ClothRed, B::ClothBlue, B::ClothYellow, B::ClothRed};
            for (int i = 0; i < 4; ++i) {
                const float a = (static_cast<float>(i) + 0.5f) * kPi * 0.5f;
                const float dx = std::cos(a), dz = std::sin(a);
                BuildContext sc = context(style, main_roof, 1500 + i);
                build::stall(sc, kCloth[i], kShops[i]);
                const int rot = rotFacing(-dx, -dz);
                const int sx = (rot & 1) ? sc.bp.d : sc.bp.w, sz = (rot & 1) ? sc.bp.w : sc.bp.d;
                const int x0 = static_cast<int>(std::lround(fcx + dx * 5.0f - static_cast<float>(sx) * 0.5f));
                const int z0 = static_cast<int>(std::lround(static_cast<float>(zc) + dz * 5.0f - static_cast<float>(sz) * 0.5f));
                add(s, sc, x0, base, z0, rot);
            }
            for (int i = 0; i < 4; ++i) {
                const int x = cx + (i % 2 ? 8 : -8), z = zc + (i < 2 ? -8 : 8);
                BuildContext lc = context(style, main_roof, 1600 + i);
                build::streetLamp(lc);
                add(s, lc, x, base, z, 0);
                lot.used.push_back(Rect{x, z, x, z});
            }
        }
        // Edificios a lo largo de la calle mayor y la transversal.
        const int lim = H - 6;
        const auto inside = [&](const Rect& r) {
            return r.x0 >= cx - lim && r.x1 <= cx + lim && r.z0 >= castle_z1 + 2 && r.z1 <= cz + lim;
        };
        enum Kind { KTavern, KSmithy, KTemple, KPotions, KArmor, KGeneral, KStable, KHouse };
        const std::vector<Kind> queue = {KTavern, KSmithy, KTemple, KStable, KArmor, KPotions, KGeneral};
        static const char* const kTags[] = {"ciudadano", "artesano", "guardia", "soldado", "noble", "panadero", "sastre", "mensajero",
                                            "nino", "capitan"};
        std::size_t next = 0;
        int placed = 0;
        struct Walk {
            float x0, z0, x1, z1, half;
        };
        const std::vector<Walk> walks = {{fcx, static_cast<float>(castle_z1 + 2), fcx, fcz + static_cast<float>(lim), 3.0f},
                                         {fcx + 8.0f, static_cast<float>(zc), fcx + ring, static_cast<float>(zc), 2.0f},
                                         {fcx - 8.0f, static_cast<float>(zc), fcx - ring, static_cast<float>(zc), 2.0f},
                                         {fcx - ring, fcz + ring, fcx + ring, fcz + ring, 2.0f}};
        for (const Walk& wk : walks) {
            const float len = std::hypot(wk.x1 - wk.x0, wk.z1 - wk.z0);
            if (len < 1.0f) continue;
            const float dx = (wk.x1 - wk.x0) / len, dz = (wk.z1 - wk.z0) / len;
            for (float t = 1.0f; t < len; t += 3.0f) {
                for (int side = -1; side <= 1; side += 2) {
                    const float px = wk.x0 + dx * t, pz = wk.z0 + dz * t;
                    const float nx = -dz * static_cast<float>(side), nz = dx * static_cast<float>(side);
                    const Kind kind = next < queue.size() ? queue[next] : KHouse;
                    BuildContext ctx = context(style, roof(), 2000 + placed * 3 + side);
                    const int hw = rng.range(7, 9), hd = rng.range(7, 8);
                    const int floors = rng.range(1, 3);
                    const std::string tag = rng.pick(kTags);
                    const auto fn = [&](BuildContext& c) {
                        switch (kind) {
                            case KTavern: build::tavern(c); break;
                            case KSmithy: build::smithy(c, true); break;
                            case KTemple: c.roof = B::RoofBlue; build::temple(c); break;
                            case KPotions: build::shop(c, "pociones"); break;
                            case KArmor: build::shop(c, "armaduras"); break;
                            case KGeneral: build::shop(c, "general"); break;
                            case KStable: build::stable(c); break;
                            default: build::house(c, hw, hd, floors, tag); break;
                        }
                    };
                    int bw = hw + 2, bd = hd + 2;
                    switch (kind) {
                        case KTavern: bw = 15; bd = 13; break;
                        case KSmithy: bw = 12; bd = 10; break;
                        case KTemple: bw = 13; bd = 21; break;
                        case KPotions: case KArmor: case KGeneral: bw = 11; bd = 10; break;
                        case KStable: bw = 13; bd = 11; break;
                        default: break;
                    }
                    if (placeFacing(s, lot, ctx, fn, bw, bd, px, pz, nx, nz, wk.half, base, inside)) {
                        ++placed;
                        if (next < queue.size()) ++next;
                    }
                }
            }
        }
        // Farolas en la calle mayor, arboles y munecos de practica en los huecos.
        for (int t = 4; t < cz + H - 6 - castle_z1; t += 9) {
            for (int side = -1; side <= 1; side += 2) {
                const int x = cx + side * 4, z = castle_z1 + 2 + t;
                const Rect r{x, z, x, z};
                if (!lot.freeRect(r, 2)) continue;  // CIUDADES: no delante de las puertas
                lot.used.push_back(r);
                BuildContext lc = context(style, main_roof, 3000 + t * 2 + side);
                build::streetLamp(lc);
                add(s, lc, x, base, z, 0);
            }
        }
        for (int i = 0, made = 0; i < 120 && made < 10; ++i) {
            const int x0 = cx + rng.range(-lim, lim - 7), z0 = cz + rng.range(-lim, lim - 7);
            const Rect r{x0, z0, x0 + 6, z0 + 6};
            if (r.z0 < castle_z1 + 2 && (r.x1 >= cx - castle_w / 2 - 2 && r.x0 <= cx + castle_w / 2 + 2)) continue;
            if (r.x0 < cx - lim || r.x1 > cx + lim || r.z0 < cz - lim || r.z1 > cz + lim) continue;
            if (!lot.freeRect(r, 0) || !lot.clearOfRoads(r, 0.0f)) continue;
            lot.used.push_back(Rect{x0 + 1, z0 + 1, x0 + 5, z0 + 5});
            BuildContext tc = context(style, main_roof, 3500 + i);
            if (made % 3 == 2) {
                build::farm(tc, 7, 7);  // huerto del castillo
            } else {
                build::tree(tc);
            }
            add(s, tc, x0, base, z0, 0);
            ++made;
        }
        addPoint(s, "npc", fcx - 3.5f, static_cast<float>(base + 1), fcz + static_cast<float>(H - 8), "guardia", 2);
        addPoint(s, "npc", fcx + 3.5f, static_cast<float>(base + 1), fcz + static_cast<float>(H - 8), "guardia", 2);
        addPoint(s, "npc", fcx + 6.5f, static_cast<float>(base + 1), static_cast<float>(zc) + 0.5f, "capitan", 3);
        outerFarms(s, lot, cx, cz, H, base, biome_style, main_roof);
        hubs.push_back({fcx, fcz + static_cast<float>(H + 14)});
        finish(s);
    }

    // --- Plazas hostiles ----------------------------------------------------------------
    // Sitio llano, seco, sin lava y lejos de todo lo demas (30 m mas de margen).
    bool hostileSite(int radius, int max_range, int& x, int& z, int& base, int& biome) {
        for (int attempt = 0; attempt < 10; ++attempt) {
            const int lim = kHalf - radius - 34;
            if (lim < 0) return false;
            x = cx0 + rng.range(-lim, lim);
            z = cz0 + rng.range(-lim, lim);
            if (!clearSpot(x, z, static_cast<float>(radius + 30))) continue;
            const SiteStats st = site(x, z, radius, 5);
            if (st.water > 0 || st.lava > 0 || st.max - st.min > max_range || st.min < sea + 1) continue;
            base = st.median;
            biome = st.biome;
            return true;
        }
        return false;
    }

    // Aplanado (cuadrado) y pieza unica girada al azar, centrada en (x, z).
    void placeHostile(Structure& s, BuildContext& ctx, int x, int z, int base, int biome) {
        const int rot = rng.range(0, 3);
        const int sx = (rot & 1) ? ctx.bp.d : ctx.bp.w, sz = (rot & 1) ? ctx.bp.w : ctx.bp.d;
        const int half = std::max(sx, sz) / 2;
        Terraform tf;
        tf.cx = x;
        tf.cz = z;
        tf.inner = static_cast<float>(half + 3);
        tf.outer = static_cast<float>(half + 16);
        tf.square = true;
        tf.base = base;
        tf.surface = hostileSurface(biome);
        tf.under = hostileUnder(biome);
        tf.decor = 0.0f;
        s.terraforms.push_back(tf);
        s.clear_radius = static_cast<float>(half + 3) * 1.42f + 8.0f;
        add(s, ctx, x - sx / 2, base, z - sz / 2, rot);
        finish(s);
    }

    static bool redLands(int biome) { return biome == Generator::ABadlands || biome == Generator::ARoseDesert; }

    bool tryBanditFort() {
        int x, z, base, biome;
        const int S = 37 + 2 * rng.range(0, 4);
        if (!hostileSite(static_cast<int>(static_cast<float>(S) * 0.71f) + 3, 9, x, z, base, biome)) return false;
        Rng nr(hashOf(seed, rx, rz, 61));
        Structure& s = begin("fuerte", banditName(nr), x, base + 1, z, levelAt(x, z, 0.8f));
        BuildContext ctx = context(BuildStyle::Temperate, B::Thatch, 6000);
        build::banditFort(ctx, S, "bandido", "bandido_arquero", "bandido");
        placeHostile(s, ctx, x, z, base, biome);
        return true;
    }

    bool tryOrcCamp() {
        int x, z, base, biome;
        const int R = 18 + rng.range(0, 5);
        if (!hostileSite(R + 8, 10, x, z, base, biome)) return false;
        Rng nr(hashOf(seed, rx, rz, 67));
        Structure& s = begin("campamento", orcName(nr), x, base + 1, z, levelAt(x, z, 1.2f));
        BuildContext ctx = context(BuildStyle::Temperate, B::Thatch, 6100);
        const std::string archer = biome == Generator::AAshlands ? "diablillo" : "goblin_arquero";
        build::orcCamp(ctx, R, "orco", archer, "caudillo_orco");
        placeHostile(s, ctx, x, z, base, biome);
        return true;
    }

    bool tryUndeadCastle() {
        int x, z, base, biome;
        const int W = 45 + 2 * rng.range(0, 4), D = 46 + rng.range(0, 8);
        if (!hostileSite(static_cast<int>(static_cast<float>(std::max(W, D)) * 0.71f) + 3, 10, x, z, base, biome)) return false;
        Rng nr(hashOf(seed, rx, rz, 71));
        const int level = levelAt(x, z, 2.0f);
        Structure& s = begin("castillo_maldito", undeadName(nr), x, base + 1, z, level);
        std::vector<std::string> garrison = {"esqueleto", "zombi", "caballero_espectral", "espectro"};
        if (biome == Generator::AAshlands) garrison[1] = "diablillo";
        // El Rey Liche (jefe de mundo) solo en algunos castillos de nivel alto; si no, el nigromante.
        const std::string boss = level >= 6 && rng.f() < 0.35f ? "rey_liche" : "nigromante";
        BuildContext ctx = context(BuildStyle::Temperate, B::RoofSlate, 6200);
        build::undeadCastle(ctx, W, D, garrison, boss);
        placeHostile(s, ctx, x, z, base, biome);
        return true;
    }

    bool tryCursedCity() {
        int x, z, base, biome;
        const int S = 65 + 2 * rng.range(0, 5);
        if (!hostileSite(static_cast<int>(static_cast<float>(S) * 0.71f) + 3, 12, x, z, base, biome)) return false;
        Rng nr(hashOf(seed, rx, rz, 73));
        Structure& s = begin("ciudad_maldita", cursedCityName(nr), x, base + 1, z, levelAt(x, z, 2.5f));
        std::vector<std::string> garrison = {"zombi", "esqueleto", "espectro", "caballero_espectral"};
        if (biome == Generator::ABlight) garrison.push_back("arana");
        const std::string boss = rng.f() < 0.5f ? "nigromante" : "caballero_espectral";
        BuildContext ctx = context(BuildStyle::Temperate, B::RoofSlate, 6300);
        build::cursedCity(ctx, S, garrison, boss);
        placeHostile(s, ctx, x, z, base, biome);
        return true;
    }

    bool tryCultTower() {
        int x, z, base, biome;
        if (!hostileSite(26, 8, x, z, base, biome)) return false;
        Rng nr(hashOf(seed, rx, rz, 79));
        Structure& s = begin("torre_culto", cultName(nr), x, base + 1, z, levelAt(x, z, 1.5f));
        std::string minion = "brujo", boss = "brujo";
        if (biome == Generator::AAshlands) minion = "diablillo";
        else if (biome == Generator::ABlight) {
            minion = "bruja_pantano";
            boss = "nigromante";
        } else if (redLands(biome)) {
            minion = rng.f() < 0.5f ? "diablillo" : "brujo";
        }
        BuildContext ctx = context(BuildStyle::Temperate, B::RoofPurple, 6400);
        build::cultistTower(ctx, 4 + rng.range(0, 1), minion, boss);
        placeHostile(s, ctx, x, z, base, biome);
        return true;
    }

    // Lo hostil de la region: nada cerca de la capital; mas y peor en las tierras
    // peligrosas (paramo de ceniza, tierras marchitas, tierras carmesi y desierto rosa).
    void tryHostile() {
        if (dimension != "superficie") return;
        const int ring = std::max(std::abs(rx), std::abs(rz));
        if (ring < 2) return;
        const SiteStats reg = site(cx0, cz0, kHalf - 40, 3);
        const bool ash = reg.biome == Generator::AAshlands, blight = reg.biome == Generator::ABlight, red = redLands(reg.biome);
        float p_bandit = 0.16f, p_orc = ring >= 3 ? 0.11f : 0.0f, p_undead = ring >= 3 ? 0.05f : 0.0f;
        float p_cursed = ring >= 4 ? 0.035f : 0.0f, p_cult = ring >= 3 ? 0.05f : 0.0f;
        if (ash) {
            p_bandit = 0.0f, p_orc = 0.3f, p_undead = 0.3f, p_cursed = 0.2f, p_cult = 0.45f;
        } else if (blight) {
            p_bandit = 0.0f, p_orc = 0.1f, p_undead = 0.45f, p_cursed = 0.35f, p_cult = 0.35f;
        } else if (red) {
            p_bandit = 0.3f, p_orc = 0.4f, p_undead = 0.08f, p_cursed = 0.06f, p_cult = 0.25f;
        }
        const int max_made = ash || blight || red ? 3 : 2;
        int made = 0;
        // CIUDADES-GRANDES: hostileSite mira un radio estimado; la plaza ya hecha puede quedar mas cerca de una
        // aldea, ciudad o metropolis (RegionPlan::shared) de 30 m entre sus circulos: entonces se quita.
        const auto keep = [&](bool ok) {
            if (!ok) return 0;
            const Structure& h = out.structures.back();
            bool near = false;
            out.forEach([&](const Structure& a) {
                if (&a == &h || (a.info.type != "aldea" && a.info.type != "ciudad" && a.info.type != "castillo" && a.info.type != "santuario")) return;
                near = near || std::hypot(a.clear_x - h.clear_x, a.clear_z - h.clear_z) < a.clear_radius + h.clear_radius + 30.0f;
            });
            if (!near) return 1;
            out.structures.pop_back();
            return 0;
        };
        if (made < max_made && rng.f() < p_cursed) made += keep(tryCursedCity());
        if (made < max_made && rng.f() < p_undead) made += keep(tryUndeadCastle());
        if (made < max_made && rng.f() < p_orc) made += keep(tryOrcCamp());
        if (made < max_made && rng.f() < p_bandit) made += keep(tryBanditFort());
        if (made < max_made && rng.f() < p_cult) made += keep(tryCultTower());
    }

    // --- Caminos entre regiones ------------------------------------------------------------
    // Cada borde entre dos regiones puede tener un cruce (punto del borde): lo
    // calculan igual las dos regiones (solo depende de la semilla y del borde). Cada
    // region une sus cruces con la entrada mas cercana de sus asentamientos (o con un
    // cruce de caminos en el campo si no tiene ninguno).
    bool crossing(int ex, int ez, bool vertical, float& x, float& z) const {
        const std::uint32_t h = hashOf(seed, ex * 2 + (vertical ? 1 : 0), ez, 0xC0AD);
        if (static_cast<float>(h & 0xFFFF) / 65536.0f > 0.62f) return false;
        const float off = (static_cast<float>((h >> 16) & 0xFFFF) / 65536.0f - 0.5f) * 0.6f * static_cast<float>(kRegionSize);
        if (vertical) {  // entre (ex, ez) y (ex + 1, ez)
            x = static_cast<float>(ex * kRegionSize + kHalf);
            z = static_cast<float>(ez * kRegionSize) + off;
        } else {  // entre (ex, ez) y (ex, ez + 1)
            x = static_cast<float>(ex * kRegionSize) + off;
            z = static_cast<float>(ez * kRegionSize + kHalf);
        }
        const auto c = col(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(z)));
        return c.height >= sea + 2 && c.height <= sea + 48 && c.river < 0.2f && !c.lake && c.lava <= c.height && !c.giant;
    }

    void planRoads() {
        if (dimension != "superficie") return;
        std::vector<std::array<float, 2>> ends;
        float x = 0, z = 0;
        if (crossing(rx - 1, rz, true, x, z)) ends.push_back({x, z});
        if (crossing(rx, rz, true, x, z)) ends.push_back({x, z});
        if (crossing(rx, rz - 1, false, x, z)) ends.push_back({x, z});
        if (crossing(rx, rz, false, x, z)) ends.push_back({x, z});
        if (ends.empty()) return;
        Rng rr(hashOf(seed, rx, rz, 0xA11E));
        std::vector<std::array<float, 2>> targets = hubs;
        if (targets.empty()) {
            if (ends.size() < 2) return;  // un camino suelto que no va a ninguna parte
            targets.push_back({static_cast<float>(cx0) + (rr.f() - 0.5f) * 120.0f, static_cast<float>(cz0) + (rr.f() - 0.5f) * 120.0f});
        }
        const float lo_x = static_cast<float>(cx0 - kHalf + 6), hi_x = static_cast<float>(cx0 + kHalf - 6);
        const float lo_z = static_cast<float>(cz0 - kHalf + 6), hi_z = static_cast<float>(cz0 + kHalf - 6);
        for (const auto& e : ends) {
            std::size_t best = 0;
            float best_d = 1e30f;
            for (std::size_t i = 0; i < targets.size(); ++i) {
                const float d = std::hypot(targets[i][0] - e[0], targets[i][1] - e[1]);
                if (d < best_d) {
                    best_d = d;
                    best = i;
                }
            }
            const auto& t = targets[best];
            // Un recodo a mitad de camino (apartado de los asentamientos).
            const float dx = t[0] - e[0], dz = t[1] - e[1];
            const float len = std::max(1.0f, std::hypot(dx, dz));
            const float bend = (rr.f() - 0.5f) * 0.35f * len;
            float mx = e[0] + dx * 0.5f - dz / len * bend, mz = e[1] + dz * 0.5f + dx / len * bend;
            out.forEach([&](const Structure& s) {
                if (s.info.type != "aldea" && s.info.type != "ciudad" && s.info.type != "castillo") return;
                const float ox = mx - s.clear_x, oz = mz - s.clear_z, od = std::hypot(ox, oz);
                if (od < s.clear_radius + 8.0f && od > 0.1f) {
                    mx = s.clear_x + ox / od * (s.clear_radius + 8.0f);
                    mz = s.clear_z + oz / od * (s.clear_radius + 8.0f);
                }
            });
            mx = std::clamp(mx, lo_x, hi_x);
            mz = std::clamp(mz, lo_z, hi_z);
            const auto c = col(static_cast<int>(mx), static_cast<int>(mz));
            BlockId surf = B::DirtPath, alt = B::Gravel;
            if (c.biome == Generator::ADesert) {
                surf = B::Sandstone;
                alt = B::Sand;
            } else if (c.biome == Generator::ABadlands) {
                surf = B::RedSandstone;
                alt = B::RedSand;
            } else if (c.biome == Generator::ARoseDesert) {
                surf = B::PinkSandstone;
                alt = B::PinkSand;
            } else if (c.biome == Generator::AAshlands) {
                surf = B::Basalt;
                alt = B::Ash;
            }
            out.roads.push_back(Road{e[0], e[1], mx, mz, 1.7f, surf, alt, 0.25f});
            out.roads.push_back(Road{mx, mz, t[0], t[1], 1.7f, surf, alt, 0.25f});
        }
    }

    // --- Torre de mago, ruinas, portal ---------------------------------------------
    bool smallSite(int radius, int max_range, int& x, int& z, int& base, int& biome) {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const int lim = kHalf - radius - 20;
            x = cx0 + rng.range(-lim, lim);
            z = cz0 + rng.range(-lim, lim);
            if (!clearSpot(x, z, static_cast<float>(radius + 10))) continue;
            const SiteStats st = site(x, z, radius, 3);
            if (st.water > 0 || st.max - st.min > max_range || st.min < sea + 1) continue;
            base = st.median;
            biome = st.biome;
            return true;
        }
        return false;
    }

    void pad(Structure& s, int x, int z, int base, float inner, float outer, BuildStyle style) {
        Terraform tf;
        tf.cx = x;
        tf.cz = z;
        tf.inner = inner;
        tf.outer = outer;
        tf.base = base;
        tf.surface = surfaceFor(style);
        tf.under = style == BuildStyle::Desert ? B::Sand : B::Dirt;
        s.terraforms.push_back(tf);
        s.clear_radius = outer + 4.0f;
    }

    void tryTower() {
        int x, z, base, biome;
        if (!smallSite(8, 5, x, z, base, biome)) return;
        Rng nr(hashOf(seed, rx, rz, 41));
        Structure& s = begin("torre", towerName(nr), x, base + 1, z, levelAt(x, z, 1.0f));
        pad(s, x, z, base, 9.0f, 16.0f, styleFor(biome));
        BuildContext ctx = context(BuildStyle::Temperate, rng.f() < 0.7f ? B::RoofPurple : B::RoofBlue, 4000);
        build::wizardTower(ctx, rng.range(3, 4));
        const int S = ctx.bp.w;
        add(s, ctx, x - S / 2, base, z - S / 2, rng.range(0, 3));
        finish(s);
    }

    void tryRuin() {
        const int size = rng.range(15, 21);
        int x, z, base, biome;
        if (!smallSite(size / 2 + 2, 6, x, z, base, biome)) return;
        Rng nr(hashOf(seed, rx, rz, 43));
        Structure& s = begin("ruina", ruinName(nr), x, base + 1, z, levelAt(x, z, 0.5f));
        pad(s, x, z, base, static_cast<float>(size / 2 + 2), static_cast<float>(size / 2 + 9), styleFor(biome));
        BuildContext ctx = context(BuildStyle::Temperate, B::RoofRed, 4100);
        build::ruin(ctx, size);
        add(s, ctx, x - size / 2, base, z - size / 2, rng.range(0, 3));
        for (StructurePoint& p : s.info.points)
            if (p.kind == "spawner") p.tag = spawnFor(biome, s.info.level, 0);
        finish(s);
    }

    void tryPortal() {
        int x, z, base, biome;
        if (!smallSite(7, 4, x, z, base, biome)) return;
        static const char* const kDims[] = {"abismo", "cielo", "cristal"};
        const std::string dim = rng.pick(kDims);
        Structure& s = begin("portal", "Gate to " + dimensionLabel(dim), x, base + 1, z, levelAt(x, z, 2.0f));
        pad(s, x, z, base, 8.0f, 14.0f, styleFor(biome));
        BuildContext ctx = context(BuildStyle::Temperate, B::RoofRed, 4200);
        build::portal(ctx, dim);
        add(s, ctx, x - ctx.bp.w / 2, base, z - ctx.bp.d / 2, rng.range(0, 3));
        finish(s);
    }

    // --- Mazmorra --------------------------------------------------------------------
    std::string spawnFor(int biome, int level, int salt) {
        static const char* const kCommon[] = {"esqueleto", "zombi", "murcielago", "arana", "limo"};
        static const char* const kForest[] = {"arana", "goblin", "goblin_arquero", "esqueleto"};
        static const char* const kDesert[] = {"diablillo", "lagarto", "esqueleto"};
        static const char* const kSnow[] = {"esqueleto", "lobo", "zombi"};
        static const char* const kHigh[] = {"orco", "brujo", "esqueleto", "golem"};
        static const char* const kAsh[] = {"diablillo", "salamandra", "golem"};
        static const char* const kBlight[] = {"esqueleto", "zombi", "arana", "espectro"};
        Rng r(hashOf(seed, rx * 31 + salt, rz, level));
        if (level >= 6 && r.f() < 0.4f) return r.pick(kHigh);
        switch (biome) {
            case Generator::AForest: return r.pick(kForest);
            case Generator::ADesert:
            case Generator::ABadlands:
            case Generator::ARoseDesert: return r.pick(kDesert);
            case Generator::AAshlands: return r.pick(kAsh);
            case Generator::ABlight: return r.pick(kBlight);
            case Generator::ASnow:
            case Generator::AMountains: return r.pick(kSnow);
            default: return r.pick(kCommon);
        }
    }

    void tryDungeon() {
        constexpr int kCell = 20, kGrid = 4;
        for (int attempt = 0; attempt < 6; ++attempt) {
            const int lim = kHalf - kCell * kGrid / 2 - 8;
            const int ox = cx0 + rng.range(-lim, lim) - kCell * kGrid / 2, oz = cz0 + rng.range(-lim, lim) - kCell * kGrid / 2;
            const int ei = rng.range(0, kGrid - 1), ej = rng.range(0, kGrid - 1);
            const int ex = ox + ei * kCell + kCell / 2, ez = oz + ej * kCell + kCell / 2;
            if (!clearSpot(ex, ez, 14.0f)) continue;
            const SiteStats here = site(ex, ez, 6, 3);
            if (here.water > 0 || here.max - here.min > 6 || here.min < sea + 2) continue;
            const SiteStats all = site(ox + kCell * kGrid / 2, oz + kCell * kGrid / 2, kCell * kGrid / 2, 5);
            const int level = levelAt(ex, ez, 1.0f);
            const int floors = level <= 3 ? 2 : 3;
            const int S = here.median;
            const int floor0 = std::min(S, all.min) - 15;
            if (floor0 - (floors - 1) * 12 < 8) continue;
            buildDungeon(ox, oz, ei, ej, S, floor0, floors, level, here.biome);
            return;
        }
    }

    void buildDungeon(int ox, int oz, int ei, int ej, int S, int floor0, int floors, int level, int biome) {
        constexpr int kCell = 20, kGrid = 4;
        const int ex = ox + ei * kCell + kCell / 2, ez = oz + ej * kCell + kCell / 2;
        Rng nr(hashOf(seed, rx, rz, 47));
        Structure& s = begin("mazmorra", dungeonName(nr), ex, S + 1, ez, level);
        pad(s, ex, ez, S, 7.0f, 13.0f, styleFor(biome));
        // Entrada en superficie y escalera de caracol hasta el primer piso.
        {
            BuildContext ctx = context(BuildStyle::Temperate, B::RoofRed, 5000);
            build::dungeonEntrance(ctx, S - floor0);
            add(s, ctx, ex - 5, S, ez - 5, 0);
            BuildContext sh = context(BuildStyle::Temperate, B::RoofRed, 5001);
            build::spiralShaft(sh, S - floor0, B::StoneBricks);
            add(s, sh, ex - 2, floor0, ez - 2, 0);
        }
        const std::string boss = [&]() {
            static const char* const kBoss[] = {"orco", "ogro", "golem", "brujo", "ogro"};
            if (biome == Generator::ASnow || biome == Generator::AMountains) return std::string(level >= 3 ? "yeti" : "orco");
            return std::string(kBoss[std::min(4, (level - 1) / 2)]);
        }();
        int arrive_i = ei, arrive_j = ej;
        int room_index = 0;
        for (int f = 0; f < floors; ++f) {
            const int fy = floor0 - f * 12;
            const bool last = f == floors - 1;
            // Celdas presentes y arbol de pasillos (DFS desde la llegada).
            std::array<bool, kGrid * kGrid> present{};
            std::array<int, kGrid * kGrid> dist{};
            std::vector<std::array<int, 4>> edges;  // i0 j0 i1 j1
            for (int tries = 0; tries < 2; ++tries) {  // si quedan muy pocas salas, todas presentes
            for (int i = 0; i < kGrid * kGrid; ++i) present[static_cast<std::size_t>(i)] = tries > 0 || rng.f() < 0.8f;
            present[static_cast<std::size_t>(arrive_i + arrive_j * kGrid)] = true;
            dist.fill(-1);
            std::vector<int> stack = {arrive_i + arrive_j * kGrid};
            dist[static_cast<std::size_t>(stack[0])] = 0;
            while (!stack.empty()) {
                const int cur = stack.back();
                const int ci = cur % kGrid, cj = cur / kGrid;
                int order[4] = {0, 1, 2, 3};
                for (int k = 3; k > 0; --k) std::swap(order[k], order[rng.range(0, k)]);
                bool moved = false;
                for (int k : order) {
                    const int ni = ci + (k == 0 ? 1 : (k == 1 ? -1 : 0)), nj = cj + (k == 2 ? 1 : (k == 3 ? -1 : 0));
                    if (ni < 0 || nj < 0 || ni >= kGrid || nj >= kGrid) continue;
                    const int n = ni + nj * kGrid;
                    if (!present[static_cast<std::size_t>(n)] || dist[static_cast<std::size_t>(n)] >= 0) continue;
                    dist[static_cast<std::size_t>(n)] = dist[static_cast<std::size_t>(cur)] + 1;
                    edges.push_back({ci, cj, ni, nj});
                    stack.push_back(n);
                    moved = true;
                    break;
                }
                if (!moved) stack.pop_back();
            }
            const int reached = static_cast<int>(std::count_if(dist.begin(), dist.end(), [](int v) { return v >= 0; }));
            if (reached >= 5) break;
            edges.clear();
            }
            // La mas lejana: bajada (o jefe en el ultimo piso; una sola puerta).
            int farthest = arrive_i + arrive_j * kGrid;
            for (int i = 0; i < kGrid * kGrid; ++i)
                if (dist[static_cast<std::size_t>(i)] > dist[static_cast<std::size_t>(farthest)]) farthest = i;
            const int far_i = farthest % kGrid, far_j = farthest / kGrid;
            const int boss_cell = last && farthest != arrive_i + arrive_j * kGrid ? farthest : -1;
            // Algun bucle extra (nunca a la sala del jefe).
            for (int k = 0; k < 3; ++k) {
                const int i = rng.range(0, kGrid - 2), j = rng.range(0, kGrid - 1);
                const int a = i + j * kGrid, b = i + 1 + j * kGrid;
                if (a == boss_cell || b == boss_cell) continue;
                if (dist[static_cast<std::size_t>(a)] >= 0 && dist[static_cast<std::size_t>(b)] >= 0) {
                    bool dup = false;
                    for (const auto& e : edges) dup = dup || (std::min(e[0], e[2]) == i && e[1] == j && e[3] == j);
                    if (!dup) edges.push_back({i, j, i + 1, j});
                }
            }
            // Salas.
            std::array<int, kGrid * kGrid> iw{}, idp{};
            bool treasure = false;
            for (int c = 0; c < kGrid * kGrid; ++c) {
                if (dist[static_cast<std::size_t>(c)] < 0) continue;
                const int ci = c % kGrid, cj = c / kGrid;
                const bool arrival = ci == arrive_i && cj == arrive_j;
                const bool down = !last && c == farthest;
                const bool boss_room = last && c == farthest && !arrival;
                int w = 9 + 2 * rng.range(0, 2), d = 9 + 2 * rng.range(0, 2), h = rng.range(4, 6);
                build::RoomKind kind = build::RoomKind::Hall;
                if (boss_room) {
                    w = d = 17;
                    h = 8;
                    kind = build::RoomKind::Boss;
                } else if (arrival || down) {
                    kind = rng.f() < 0.5f ? build::RoomKind::Hall : build::RoomKind::Crypt;
                    h = std::max(h, 5);
                } else {
                    const float r = rng.f();
                    if (r < 0.16f) kind = build::RoomKind::Library;
                    else if (r < 0.32f) kind = build::RoomKind::Crypt;
                    else if (r < 0.45f) kind = build::RoomKind::Prison;
                    else if (r < 0.57f) kind = build::RoomKind::Lava;
                    else if (r < 0.66f) kind = build::RoomKind::Flooded;
                    else if (r < 0.77f) kind = build::RoomKind::Trap;
                    else if (r < 0.87f && !treasure) {
                        kind = build::RoomKind::Treasure;
                        treasure = true;
                    }
                }
                iw[static_cast<std::size_t>(c)] = w;
                idp[static_cast<std::size_t>(c)] = d;
                std::string spawn = spawnFor(biome, level, c + f * 16);
                if (kind == build::RoomKind::Lava) spawn = "diablillo";
                else if (kind == build::RoomKind::Flooded) spawn = "rana";
                else if (kind == build::RoomKind::Crypt) spawn = rng.f() < 0.5f ? "esqueleto" : "zombi";
                else if (kind == build::RoomKind::Library && level >= 3) spawn = "brujo";
                BuildContext ctx = context(BuildStyle::Temperate, B::RoofRed, 5100 + c + f * 31);
                build::dungeonRoom(ctx, w, h, d, kind, level, arrival && f == 0 ? std::string() : spawn, boss, arrival ? 1 : (down ? 2 : 0));
                const int rcx = ox + ci * kCell + kCell / 2, rcz = oz + cj * kCell + kCell / 2;
                const int x0 = rcx - (w + 1) / 2, z0 = rcz - (d + 1) / 2;
                add(s, ctx, x0, fy, z0, 0);
                s.rooms.push_back(Structure::Room{x0, fy, z0, x0 + w + 1, fy + h + 1, z0 + d + 1});
                ++room_index;
                if (down) {
                    BuildContext sh = context(BuildStyle::Temperate, B::RoofRed, 5300 + f);
                    build::spiralShaft(sh, 12, B::DungeonBricks);
                    add(s, sh, rcx - 2, fy - 12, rcz - 2, 0);
                }
            }
            // Pasillos.
            int ci_edge = 0;
            for (const auto& e : edges) {
                const int a = e[0] + e[1] * kGrid, b = e[2] + e[3] * kGrid;
                const bool along_x = e[1] == e[3];
                const int first = along_x ? (e[0] < e[2] ? a : b) : (e[1] < e[3] ? a : b);
                const int second = first == a ? b : a;
                const int fi = first % kGrid, fj = first / kGrid, si = second % kGrid, sj = second / kGrid;
                BuildContext ctx = context(BuildStyle::Temperate, B::RoofRed, 5500 + ci_edge++ + f * 61);
                if (along_x) {
                    const int rcz = oz + fj * kCell + kCell / 2;
                    const int wall_a = ox + fi * kCell + kCell / 2 + (iw[static_cast<std::size_t>(first)] + 1) / 2;
                    const int wall_b = ox + si * kCell + kCell / 2 - (iw[static_cast<std::size_t>(second)] + 1) / 2;
                    const int len = wall_b - wall_a + 1;
                    if (len < 1) continue;
                    build::corridor(ctx, len, rng.f() < 0.2f && first != boss_cell && second != boss_cell);
                    add(s, ctx, wall_a, fy, rcz - 2, 1);
                    // Hueco de la puerta de la sala del jefe (el script la tapia en la pelea).
                    if (second == boss_cell) addPoint(s, "puerta_jefe", static_cast<float>(wall_b) + 0.5f, static_cast<float>(fy + 1), static_cast<float>(rcz) + 0.5f, "", 1);
                    if (first == boss_cell) addPoint(s, "puerta_jefe", static_cast<float>(wall_a) + 0.5f, static_cast<float>(fy + 1), static_cast<float>(rcz) + 0.5f, "", 3);
                } else {
                    const int rcx = ox + fi * kCell + kCell / 2;
                    const int wall_a = oz + fj * kCell + kCell / 2 + (idp[static_cast<std::size_t>(first)] + 1) / 2;
                    const int wall_b = oz + sj * kCell + kCell / 2 - (idp[static_cast<std::size_t>(second)] + 1) / 2;
                    const int len = wall_b - wall_a + 1;
                    if (len < 1) continue;
                    build::corridor(ctx, len, rng.f() < 0.2f && first != boss_cell && second != boss_cell);
                    add(s, ctx, rcx - 2, fy, wall_a, 0);
                    if (second == boss_cell) addPoint(s, "puerta_jefe", static_cast<float>(rcx) + 0.5f, static_cast<float>(fy + 1), static_cast<float>(wall_b) + 0.5f, "", 2);
                    if (first == boss_cell) addPoint(s, "puerta_jefe", static_cast<float>(rcx) + 0.5f, static_cast<float>(fy + 1), static_cast<float>(wall_a) + 0.5f, "", 0);
                }
            }
            arrive_i = far_i;
            arrive_j = far_j;
        }
        (void)room_index;
        finish(s);
    }

#include "StructureMetropolis.inc"  // CIUDADES-GRANDES
};

}  // namespace

// ================================================================================

Structures::Structures(const Generator& generator, std::string dimension) : gen_(generator), dimension_(std::move(dimension)) {}

int Structures::regionOf(int coordinate) { return floorDiv(coordinate + kHalf, kRegionSize); }

std::shared_ptr<const RegionPlan> Structures::region(int rx, int rz) const {
    const std::int64_t key = regionKey(rx, rz);
    std::promise<std::shared_ptr<const RegionPlan>> promise;
    std::shared_future<std::shared_ptr<const RegionPlan>> future;
    {
        std::lock_guard lock(mutex_);
        const auto it = cache_.find(key);
        if (it != cache_.end()) {
            future = it->second;
        } else {
            // Lo planifica este hilo (fuera del candado); los demas esperan su futuro.
            cache_.emplace(key, promise.get_future().share());
            order_.push_back(key);
            while (order_.size() > 24) {
                cache_.erase(order_.front());
                order_.pop_front();
            }
        }
    }
    if (future.valid()) return future.get();
    std::shared_ptr<const RegionPlan> plan = planRegion(rx, rz);
    promise.set_value(plan);
    return plan;
}

std::shared_ptr<const RegionPlan> Structures::planRegion(int rx, int rz) const {
    auto plan = std::make_shared<RegionPlan>();
    plan->rx = rx;
    plan->rz = rz;
    const std::uint32_t world_seed = hashOf(static_cast<std::uint32_t>(gen_.seed()), gen_.seaLevel(), 0x51);
    // CIUDADES-GRANDES: la metropolis de la celda de 3 x 3 regiones, si toca esta region (solo en
    // las regiones amplias: las kAmpleRing del origen no cambian).
    const bool ample = std::max(std::abs(rx), std::abs(rz)) >= kAmpleRing;
    if (ample && dimension_ == "superficie") {
        if (std::shared_ptr<const Structure> m = metropolis(megaOf(rx), megaOf(rz))) {
            const float x0 = static_cast<float>(rx * kRegionSize - kHalf), x1 = static_cast<float>(rx * kRegionSize + kHalf);
            const float z0 = static_cast<float>(rz * kRegionSize - kHalf), z1 = static_cast<float>(rz * kRegionSize + kHalf);
            const float qx = std::clamp(m->clear_x, x0, x1), qz = std::clamp(m->clear_z, z0, z1);
            const float reach = m->clear_radius + 20.0f;
            if ((qx - m->clear_x) * (qx - m->clear_x) + (qz - m->clear_z) * (qz - m->clear_z) < reach * reach) plan->shared.push_back(m);
        }
    }
    Planner pl(gen_, dimension_, world_seed, rx, rz, *plan);
    for (const auto& m : plan->shared)
        for (const auto& h : m->hubs)
            if (regionOf(static_cast<int>(std::floor(h[0]))) == rx && regionOf(static_cast<int>(std::floor(h[1]))) == rz) pl.hubs.push_back(h);
    if (rx == 0 && rz == 0) {
        pl.buildCapital();  // siempre la capital en el origen
        pl.planRoads();     // caminos hacia las regiones de al lado (fuera de la capital)
        return plan;
    }
    const float r = pl.rng.f();
    bool settled = false;
    if (r < 0.08f) settled = pl.tryCity();
    else if (r < 0.14f) settled = pl.tryRoyalCastle();  // castillo real con su patio de armas
    if (!settled && r < 0.55f) settled = pl.tryVillage();
    if (pl.rng.f() < 0.55f) pl.tryDungeon();
    if (pl.rng.f() < 0.18f) pl.tryTower();
    if (pl.rng.f() < 0.35f) pl.tryRuin();
    if (pl.rng.f() < 0.07f) pl.tryPortal();
    pl.tryHostile();  // fuertes de bandidos, campamentos orcos, castillos no muertos...
    // CIUDADES-GRANDES: castillos en las colinas y santuarios misteriosos (azar aparte: lo de antes
    // de la region no cambia por ellos).
    if (ample && dimension_ == "superficie") {
        Rng xr(hashOf(world_seed, rx, rz, 0xC457));
        if (xr.f() < 0.14f) pl.tryHillCastle(xr);
        if (xr.f() < 0.30f) pl.tryShrine(xr);
    }
    pl.planRoads();
    return plan;
}

int Structures::megaOf(int region) { return floorDiv(region + 1, 3); }

std::shared_ptr<const Structure> Structures::metropolis(int mi, int mj) const {
    if (mi == 0 && mj == 0) return nullptr;
    const std::int64_t key = regionKey(mi, mj);
    std::promise<std::shared_ptr<const Structure>> promise;
    std::shared_future<std::shared_ptr<const Structure>> future;
    {
        std::lock_guard lock(metro_mutex_);
        const auto it = metro_cache_.find(key);
        if (it != metro_cache_.end()) {
            future = it->second;
        } else {
            metro_cache_.emplace(key, promise.get_future().share());
            metro_order_.push_back(key);
            while (metro_order_.size() > 8) {
                metro_cache_.erase(metro_order_.front());
                metro_order_.pop_front();
            }
        }
    }
    if (future.valid()) return future.get();
    std::shared_ptr<const Structure> m = planMetropolis(mi, mj);
    promise.set_value(m);
    return m;
}

std::shared_ptr<const Structure> Structures::planMetropolis(int mi, int mj) const {
    RegionPlan tmp;
    tmp.rx = 3 * mi;
    tmp.rz = 3 * mj;
    const std::uint32_t world_seed = hashOf(static_cast<std::uint32_t>(gen_.seed()), gen_.seaLevel(), 0x51);
    Planner pl(gen_, dimension_, world_seed, 3 * mi, 3 * mj, tmp);
    if (!pl.buildMetropolis(mi, mj) || tmp.structures.empty()) return nullptr;
    return std::make_shared<const Structure>(std::move(tmp.structures.front()));
}

std::vector<GeneratedChest> Structures::chests(int x0, int z0, int x1, int z1) const {
    std::vector<GeneratedChest> out;
    const int r0x = regionOf(x0), r1x = regionOf(x1), r0z = regionOf(z0), r1z = regionOf(z1);
    if (r1x - r0x > 8 || r1z - r0z > 8) return out;
    std::vector<std::uint64_t> seen;
    for (int rz = r0z; rz <= r1z; ++rz)
        for (int rx = r0x; rx <= r1x; ++rx) {
            const std::shared_ptr<const RegionPlan> plan = region(rx, rz);
            plan->forEach([&](const Structure& s) {
                if (static_cast<float>(x1 + 1) < s.info.min.x || static_cast<float>(x0) > s.info.max.x || static_cast<float>(z1 + 1) < s.info.min.z ||
                    static_cast<float>(z0) > s.info.max.z)
                    return;
                if (std::find(seen.begin(), seen.end(), s.info.id) != seen.end()) return;
                seen.push_back(s.info.id);
                for (const StructurePoint& p : s.info.points) {
                    if (p.kind != "cofre_botin") continue;
                    const int x = static_cast<int>(std::floor(p.position.x)), y = static_cast<int>(std::floor(p.position.y)),
                              z = static_cast<int>(std::floor(p.position.z));
                    if (x < x0 || x > x1 || z < z0 || z > z1) continue;
                    GeneratedChest c;
                    c.x = x;
                    c.y = y;
                    c.z = z;
                    const std::size_t colon = p.tag.find(':');
                    c.tier = std::clamp(std::atoi(p.tag.c_str()), 1, 4);
                    c.table = colon == std::string::npos ? std::string() : p.tag.substr(colon + 1);
                    c.structure = s.info.id;
                    c.dimension = dimension_;
                    c.key = dimension_ + ":" + std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z);
                    out.push_back(std::move(c));
                }
            });
        }
    return out;
}

// --- Estampado ----------------------------------------------------------------------------

void Structures::stamp(ChunkData& chunk, const std::array<int, kChunkSize * kChunkSize>* heights) const {
    const int x0 = chunk.cx * kChunkSize, z0 = chunk.cz * kChunkSize;
    const int x1 = x0 + kChunkSize - 1, z1 = z0 + kChunkSize - 1;
    const std::shared_ptr<const RegionPlan> plan = region(regionOf(x0), regionOf(z0));
    std::vector<const Structure*> touching;
    plan->forEach([&](const Structure& s) {
        float reach = 0.0f;
        for (const Terraform& tf : s.terraforms) reach = std::max(reach, tf.outer + 2.0f);
        const int mx0 = std::min(s.min_x, static_cast<int>(s.clear_x - reach)), mx1 = std::max(s.max_x, static_cast<int>(s.clear_x + reach));
        const int mz0 = std::min(s.min_z, static_cast<int>(s.clear_z - reach)), mz1 = std::max(s.max_z, static_cast<int>(s.clear_z + reach));
        if (mx1 < x0 || mx0 > x1 || mz1 < z0 || mz0 > z1) return;
        touching.push_back(&s);
    });
    // Caminos entre regiones que pasan por el chunk.
    const auto roadTouches = [&](const Road& road) {
        const float minx = std::min(road.x0, road.x1) - road.half_width - 1.0f, maxx = std::max(road.x0, road.x1) + road.half_width + 1.0f;
        const float minz = std::min(road.z0, road.z1) - road.half_width - 1.0f, maxz = std::max(road.z0, road.z1) + road.half_width + 1.0f;
        return !(maxx < static_cast<float>(x0) || minx > static_cast<float>(x1 + 1) || maxz < static_cast<float>(z0) || minz > static_cast<float>(z1 + 1));
    };
    std::vector<const Road*> region_roads;
    for (const Road& road : plan->roads)
        if (roadTouches(road)) region_roads.push_back(&road);
    if (touching.empty() && region_roads.empty()) return;
    const auto at = [&](int x, int y, int z) -> BlockId& { return chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))]; };
    const int sea = gen_.seaLevel();
    std::array<int, kChunkSize * kChunkSize> top{};
    top.fill(-1);
    const auto findTop = [&](int lx, int lz) {
        int& t = top[static_cast<std::size_t>(lx + lz * kChunkSize)];
        if (t >= 0) return t;
        // Desde un poco por encima del suelo natural (no desde las islas flotantes).
        const int from = heights ? std::clamp((*heights)[static_cast<std::size_t>(lx + lz * kChunkSize)] + 6, 1, kWorldHeight - 1) : kWorldHeight - 1;
        for (int y = from; y > 0; --y) {
            if (groundLike(at(lx, y, lz))) {
                t = y;
                return t;
            }
        }
        t = 0;
        return t;
    };
    // 1. Aplanar el terreno.
    for (const Structure* s : touching) {
        for (const Terraform& tf : s->terraforms) {
            for (int lz = 0; lz < kChunkSize; ++lz) {
                for (int lx = 0; lx < kChunkSize; ++lx) {
                    const int wx = x0 + lx, wz = z0 + lz;
                    const float w = terraformWeight(tf, wx, wz);
                    if (w <= 0.0f) continue;
                    const int natural = findTop(lx, lz);
                    // LADERAS: con suelo propio, su altura en vez de la de la plaza.
                    const int goal = Planner::groundOr(*s, wx, wz, tf.base);
                    const int target = std::clamp(static_cast<int>(std::lround(static_cast<float>(natural) + (static_cast<float>(goal) - static_cast<float>(natural)) * w)), 2, kWorldHeight - 40);
                    const BlockId old_top = at(lx, natural, lz);
                    BlockId surface = soilSurface(old_top) && w < 0.99f ? old_top : tf.surface;
                    if (surface == B::Dirt) surface = tf.surface;
                    if (target < sea) surface = old_top == B::Sand || old_top == B::Gravel || old_top == B::Clay ? old_top : B::Sand;
                    if (target > natural) {
                        for (int y = natural + 1; y < target; ++y) at(lx, y, lz) = tf.under;
                        at(lx, target, lz) = surface;
                        // Plantas que quedaron encima del suelo viejo.
                        for (int y = target + 1; y <= target + 2 && y < kWorldHeight; ++y) {
                            const BlockId b = at(lx, y, lz);
                            if (b != B::Air && blockDef(b).shape == BlockShape::Cross) at(lx, y, lz) = B::Air;
                        }
                    } else {
                        for (int y = target + 1; y <= std::min(natural + 3, kWorldHeight - 1); ++y) {
                            at(lx, y, lz) = y < sea ? B::Water : B::Air;
                        }
                        at(lx, target, lz) = surface;
                        if (target - 1 > 0 && !groundLike(at(lx, target - 1, lz))) at(lx, target - 1, lz) = tf.under;
                    }
                    // Lo llano queda seco: un lago o un rio por encima del suelo nuevo (el
                    // aplanado rellena su fondo, pero el agua de encima seguia ahi, dentro de
                    // las casas) y las cortinas de las cascadas. Solo en lo llano: en la rampa
                    // el agua es la orilla del lago.
                    if (w >= 0.99f) {
                        for (int y = target + 1; y < std::min(target + 48, kWorldHeight); ++y) {
                            BlockId& b = at(lx, y, lz);
                            if (B::isWater(b) || b == B::FallingWater) {
                                if (y >= sea) b = B::Air;
                            } else if (b != B::Air && blockDef(b).solid) {
                                break;  // una isla flotante encima
                            }
                        }
                    }
                    if (s->seal && w >= 0.99f) {
                        for (int y = target - 1; y >= std::max(2, target - 8); --y) {
                            BlockId& b = at(lx, y, lz);
                            if (!blockDef(b).solid || B::isWater(b) || b == B::Lava) b = y >= target - 3 ? tf.under : B::Stone;
                        }
                    }
                    top[static_cast<std::size_t>(lx + lz * kChunkSize)] = target;
                    // Hierba y flores sueltas en lo plano.
                    if (w >= 0.99f && tf.decor > 0.0f && target + 1 < kWorldHeight && at(lx, target + 1, lz) == B::Air &&
                        (surface == B::Grass || surface == B::SnowyGrass || surface == B::Sand)) {
                        const std::uint32_t h = hashOf(static_cast<std::uint32_t>(tf.base), wx, wz, 0xD3C0);
                        const float r = static_cast<float>(h & 0xFFFF) / 65536.0f;
                        if (r < tf.decor) {
                            static constexpr BlockId kGrassy[] = {B::TallGrass, B::TallGrass, B::TallGrass, B::RedFlower,
                                                                  B::YellowFlower, B::WhiteFlower, B::BlueFlower, B::Bush};
                            BlockId plant = kGrassy[(h >> 16) % 8];
                            if (surface == B::SnowyGrass) plant = B::Fern;
                            if (surface == B::Sand) plant = B::DryGrass;
                            at(lx, target + 1, lz) = plant;
                        }
                    }
                }
            }
        }
    }
    // 1b. LADERAS: muros de contencion. Donde el suelo propio cae 2 o mas al lado
    // (en lo ya llano), la cara de la celda alta es de piedra; con 3 o mas, en las
    // calles y la plaza, una valla al borde. Solo con datos del plan (igual en
    // cualquier orden de carga).
    for (const Structure* s : touching) {
        if (s->ground.empty() || s->terraforms.empty()) continue;
        const Terraform& tf = s->terraforms.front();
        for (int lz = 0; lz < kChunkSize; ++lz) {
            for (int lx = 0; lx < kChunkSize; ++lx) {
                const int wx = x0 + lx, wz = z0 + lz;
                int t = 0;
                std::uint8_t kind = 0;
                if (!s->groundAt(wx, wz, t, &kind) || terraformWeight(tf, wx, wz) < 0.99f) continue;
                int low = t;
                static constexpr int kN[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : kN) {
                    int nh = 0;
                    if (s->groundAt(wx + d[0], wz + d[1], nh) && terraformWeight(tf, wx + d[0], wz + d[1]) >= 0.99f) low = std::min(low, nh);
                }
                if (t - low < 2) continue;
                for (int y = std::max(low + 1, 1); y < t; ++y) {
                    const std::uint32_t h = hashOf(0x3A11u, wx, wz, y);
                    const float r = static_cast<float>(h & 0xFFFF) / 65536.0f;
                    at(lx, y, lz) = r < 0.55f ? B::Cobblestone : (r < 0.85f ? B::StoneBricks : B::MossyBricks);
                }
                if (t - low >= 3 && t + 1 < kWorldHeight && (kind == Structure::GRoad || kind == Structure::GStep || kind == Structure::GPlaza) &&
                    !blockDef(at(lx, t + 1, lz)).solid)
                    at(lx, t + 1, lz) = B::Fence;
            }
        }
    }
    // 2. Calles y caminos sobre el suelo. Primero los caminos entre regiones, que
    // no entran en lo aplanado de ninguna estructura (dentro mandan sus calles).
    const auto paintRoad = [&](const Road& road, bool outside_only, const Structure* owner) {
            for (int lz = 0; lz < kChunkSize; ++lz) {
                for (int lx = 0; lx < kChunkSize; ++lx) {
                    const float px = static_cast<float>(x0 + lx) + 0.5f, pz = static_cast<float>(z0 + lz) + 0.5f;
                    const float d = roadDistance(road, px, pz);
                    if (d > road.half_width) continue;
                    if (outside_only) {
                        bool flat = false;
                        for (const Structure* s : touching)
                            for (const Terraform& tf : s->terraforms) flat = flat || terraformWeight(tf, x0 + lx, z0 + lz) > 0.95f;
                        if (flat) continue;
                    }
                    const int t = findTop(lx, lz);
                    if (t < sea - 1 || t <= 1) continue;  // sin puentes
                    if (outside_only && t + 1 < kWorldHeight) {  // ni por el fondo de rios, lagos o lava
                        const BlockId above = at(lx, t + 1, lz);
                        if (B::isWater(above) || above == B::Lava || above == B::PoolWater) continue;
                    }
                    const std::uint32_t h = hashOf(0xA0Du, x0 + lx, z0 + lz, t);
                    const float r = static_cast<float>(h & 0xFFFF) / 65536.0f;
                    // Borde irregular en los caminos de tierra.
                    if (road.surface == B::DirtPath && d > road.half_width - 0.6f && r < 0.5f) continue;
                    at(lx, t, lz) = (road.alt != B::Air && r < road.alt_chance) ? road.alt : road.surface;
                    // LADERAS: los escalones de las calles en ladera, de piedra.
                    std::uint8_t gk = 0;
                    int gh = 0;
                    if (owner && owner->groundAt(x0 + lx, z0 + lz, gh, &gk) && gk == Structure::GStep) at(lx, t, lz) = B::StoneBricks;
                    for (int y = t + 1; y <= t + 2 && y < kWorldHeight; ++y) {
                        const BlockId b = at(lx, y, lz);
                        if (b != B::Air && blockDef(b).shape == BlockShape::Cross) at(lx, y, lz) = B::Air;
                    }
                }
            }
    };
    for (const Road* road : region_roads) paintRoad(*road, true, nullptr);
    for (const Structure* s : touching)
        for (const Road& road : s->roads)
            if (roadTouches(road)) paintRoad(road, false, s);
    // 2b. CIUDADES-GRANDES: murallas en angulo (sobre el suelo de cada columna). Las torres y
    // puertas son piezas: van despues y abren el paso.
    for (const Structure* s : touching) {
        for (const WallLine& wl : s->walls) {
            const float reach = wl.half + 1.0f;
            if (std::max(wl.x0, wl.x1) + reach < static_cast<float>(x0) || std::min(wl.x0, wl.x1) - reach > static_cast<float>(x1 + 1) ||
                std::max(wl.z0, wl.z1) + reach < static_cast<float>(z0) || std::min(wl.z0, wl.z1) - reach > static_cast<float>(z1 + 1))
                continue;
            const float vx = wl.x1 - wl.x0, vz = wl.z1 - wl.z0;
            const float len2 = vx * vx + vz * vz;
            const float len = std::sqrt(len2);
            if (len < 0.5f) continue;
            for (int lz = 0; lz < kChunkSize; ++lz) {
                for (int lx = 0; lx < kChunkSize; ++lx) {
                    const float px = static_cast<float>(x0 + lx) + 0.5f, pz = static_cast<float>(z0 + lz) + 0.5f;
                    const float u = ((px - wl.x0) * vx + (pz - wl.z0) * vz) / len2;
                    if (u < -0.5f / len || u > 1.0f + 0.5f / len) continue;
                    // Lado: > 0 fuera (a la izquierda del sentido del tramo).
                    const float side = ((px - wl.x0) * vz - (pz - wl.z0) * vx) / len;
                    if (std::abs(side) > wl.half + 0.5f) continue;
                    int gh = findTop(lx, lz);
                    s->groundAt(x0 + lx, z0 + lz, gh);
                    const int top = gh + wl.height;
                    const bool outer = side > wl.half - 0.5f;
                    for (int y = std::max(1, gh - 3); y <= top && y < kWorldHeight; ++y) {
                        const std::uint32_t h = hashOf(0x3A12u, x0 + lx, z0 + lz, y);
                        const float r = static_cast<float>(h & 0xFFFF) / 65536.0f;
                        BlockId m = y <= gh + 1 ? (r < 0.4f ? B::MossyBricks : B::Cobblestone)
                                                : (r < 0.07f ? B::CrackedBricks : (r < 0.13f ? B::MossyBricks : B::StoneBricks));
                        if (y == top - 1 && outer) m = B::CarvedStone;
                        if (y == top && !outer) m = B::Paving;  // adarve
                        at(lx, y, lz) = m;
                    }
                    // Almenas por fuera y faroles de cuando en cuando por dentro.
                    const int k = static_cast<int>(std::floor(u * len));
                    if (outer && top + 1 < kWorldHeight) {
                        at(lx, top + 1, lz) = (k % 2 == 0) ? B::StoneBricks : B::Air;
                    } else if (side < -wl.half + 0.5f && top + 1 < kWorldHeight && k % 12 == 6) {
                        at(lx, top + 1, lz) = B::Lantern;
                    }
                }
            }
        }
    }
    // 3. Piezas: primero lo solido de todas, luego lo que no es solido (aire,
    // puertas, faroles, agua...): los pasillos abren las paredes de las salas.
    for (int pass = 0; pass < 2; ++pass) {
        for (const Structure* s : touching) {
            for (const Piece& p : s->pieces) {
                if (p.max_x < x0 || p.min_x > x1 || p.max_z < z0 || p.min_z > z1) continue;
                const Blueprint& bp = *p.plan;
                const int ax0 = std::max(p.min_x, x0), ax1 = std::min(p.max_x, x1);
                const int az0 = std::max(p.min_z, z0), az1 = std::min(p.max_z, z1);
                for (int wz = az0; wz <= az1; ++wz) {
                    for (int wx = ax0; wx <= ax1; ++wx) {
                        int lx = 0, lz = 0;
                        toLocal(p, wx, wz, lx, lz);
                        for (int ly = 0; ly < bp.h; ++ly) {
                            const BlockId id = bp.cells[bp.index(lx, ly, lz)];
                            if (id == kKeep) continue;
                            if ((pass == 0) != blockDef(id).solid) continue;
                            const int y = p.y + ly;
                            if (y < 1 || y >= kWorldHeight) continue;
                            at(wx - x0, y, wz - z0) = id;
                        }
                    }
                }
            }
        }
    }
}

bool Structures::blocksTrees(int x, int z, float margin) const {
    if (margin <= 0.0f) {
        const std::shared_ptr<const RegionPlan> plan = region(regionOf(x), regionOf(z));
        bool hit = false;
        plan->forEach([&](const Structure& s) {
            const float dx = static_cast<float>(x) - s.clear_x, dz = static_cast<float>(z) - s.clear_z;
            if (dx * dx + dz * dz < s.clear_radius * s.clear_radius) hit = true;
        });
        if (hit) return true;
        // Ni encima de los caminos entre regiones.
        const float px = static_cast<float>(x), pz = static_cast<float>(z);
        for (const Road& road : plan->roads)
            if (roadDistance(road, px, pz) < road.half_width + 1.5f) return true;
        return false;
    }
    // Con margen: el circulo mas grande y ademas los caminos (con su margen), mirando
    // tambien la region de al lado si el margen la alcanza.
    const int m = static_cast<int>(std::ceil(margin));
    const int r0x = regionOf(x - m), r1x = regionOf(x + m), r0z = regionOf(z - m), r1z = regionOf(z + m);
    const float px = static_cast<float>(x), pz = static_cast<float>(z);
    for (int rz = r0z; rz <= r1z; ++rz) {
        for (int rx = r0x; rx <= r1x; ++rx) {
            const std::shared_ptr<const RegionPlan> plan = region(rx, rz);
            bool hit = false;
            plan->forEach([&](const Structure& s) {
                if (hit) return;
                const float dx = px - s.clear_x, dz = pz - s.clear_z;
                const float r = s.clear_radius + margin;
                if (dx * dx + dz * dz < r * r) {
                    hit = true;
                    return;
                }
                // CIUDADES-GRANDES: lejos de la caja de sus calles, ni mirarlas.
                if (px < s.road_x0 - margin || px > s.road_x1 + margin || pz < s.road_z0 - margin || pz > s.road_z1 + margin) return;
                for (const Road& road : s.roads) {
                    if (roadDistance(road, px, pz) < road.half_width + margin) {
                        hit = true;
                        return;
                    }
                }
            });
            if (hit) return true;
            for (const Road& road : plan->roads)
                if (roadDistance(road, px, pz) < road.half_width + margin) return true;
        }
    }
    return false;
}

int Structures::groundHeight(int x, int z, int natural) const {
    const std::shared_ptr<const RegionPlan> plan = region(regionOf(x), regionOf(z));
    int h = natural;
    plan->forEach([&](const Structure& s) {
        for (const Terraform& tf : s.terraforms) {
            const float w = terraformWeight(tf, x, z);
            if (w <= 0.0f) continue;
            const int goal = Planner::groundOr(s, x, z, tf.base);  // LADERAS
            h = static_cast<int>(std::lround(static_cast<float>(h) + (static_cast<float>(goal) - static_cast<float>(h)) * w));
        }
    });
    return h;
}

// --- Consultas ----------------------------------------------------------------------------

std::vector<StructureInfo> Structures::nearby(float x, float y, float z, float radius) const {
    std::vector<StructureInfo> out;
    radius = std::max(radius, 0.0f);
    const int r0x = regionOf(static_cast<int>(std::floor(x - radius))), r1x = regionOf(static_cast<int>(std::floor(x + radius)));
    const int r0z = regionOf(static_cast<int>(std::floor(z - radius))), r1z = regionOf(static_cast<int>(std::floor(z + radius)));
    if (r1x - r0x > 8 || r1z - r0z > 8) return out;  // demasiado grande
    for (int rz = r0z; rz <= r1z; ++rz) {
        for (int rx = r0x; rx <= r1x; ++rx) {
            const std::shared_ptr<const RegionPlan> plan = region(rx, rz);
            plan->forEach([&](const Structure& s) {
                const float qx = std::clamp(x, s.info.min.x, s.info.max.x);
                const float qy = std::clamp(y, s.info.min.y, s.info.max.y);
                const float qz = std::clamp(z, s.info.min.z, s.info.max.z);
                const float d2 = (x - qx) * (x - qx) + (y - qy) * (y - qy) + (z - qz) * (z - qz);
                if (d2 > radius * radius) return;
                for (const StructureInfo& o : out)
                    if (o.id == s.info.id) return;  // CIUDADES-GRANDES: una metropolis toca varias regiones
                out.push_back(s.info);
            });
        }
    }
    return out;
}

bool Structures::at(float x, float y, float z, StructureInfo& out) const {
    const std::shared_ptr<const RegionPlan> plan = region(regionOf(static_cast<int>(std::floor(x))), regionOf(static_cast<int>(std::floor(z))));
    const Structure* best = nullptr;
    int best_room = -1;
    float best_volume = 1e30f;
    plan->forEach([&](const Structure& s) {
        if (x < s.info.min.x || x >= s.info.max.x || y < s.info.min.y || y >= s.info.max.y || z < s.info.min.z || z >= s.info.max.z) return;
        int room = -1;
        for (std::size_t i = 0; i < s.rooms.size(); ++i) {
            const Structure::Room& r = s.rooms[i];
            if (x >= static_cast<float>(r.x0) && x < static_cast<float>(r.x1 + 1) && y >= static_cast<float>(r.y0) &&
                y < static_cast<float>(r.y1 + 1) && z >= static_cast<float>(r.z0) && z < static_cast<float>(r.z1 + 1)) {
                room = static_cast<int>(i);
                break;
            }
        }
        const core::Vec3 size = s.info.max - s.info.min;
        float volume = size.x * size.y * size.z;
        if (room >= 0) volume = 0.0f;  // dentro de una sala: manda la mazmorra
        if (volume < best_volume) {
            best_volume = volume;
            best = &s;
            best_room = room;
        }
    });
    if (best == nullptr) return false;
    out = best->info;
    out.room = best->info.type == "mazmorra" ? best_room : -1;
    return true;
}

}  // namespace cramion::voxel
