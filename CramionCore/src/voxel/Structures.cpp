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
//   - Las demas: a veces una ciudad (rara) o una aldea (en prados llanos; de
//     playa, nieve o desierto segun el bioma), y ademas mazmorras, torres de
//     mago, ruinas y portales.
//   - Todo sale de un generador de azar sembrado con (semilla, region): el
//     mismo plan en cualquier hilo y orden.

#include "Structures.h"

#include <algorithm>
#include <array>
#include <cmath>
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

std::string dimensionLabel(const std::string& dim) {
    if (dim == "abismo") return "the Cinderdeep";
    if (dim == "cielo") return "the Driftreach";
    if (dim == "cristal") return "the Glimmerwood";
    return dim;
}

// --- Planificador de una region ---------------------------------------------------------

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

    struct SiteStats {
        int min = 999, max = -999, median = 64;
        int water = 0, samples = 0;
        int biome = Generator::AMeadow;
        float temperature = 0.0f;
    };
    SiteStats site(int cx, int cz, int radius, int grid) const {
        SiteStats s;
        std::vector<int> heights;
        std::array<int, 8> biomes{};
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
                ++s.samples;
            }
        }
        std::sort(heights.begin(), heights.end());
        s.median = heights[heights.size() / 2];
        s.biome = static_cast<int>(std::max_element(biomes.begin(), biomes.end()) - biomes.begin());
        s.temperature /= static_cast<float>(std::max(1, s.samples));
        return s;
    }

    static BuildStyle styleFor(int biome) {
        switch (biome) {
            case Generator::ASnow:
            case Generator::AMountains: return BuildStyle::Snow;
            case Generator::ADesert: return BuildStyle::Desert;
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
        for (const Structure& s : out.structures) {
            const float dx = static_cast<float>(x) - s.clear_x, dz = static_cast<float>(z) - s.clear_z;
            if (std::sqrt(dx * dx + dz * dz) < r + s.clear_radius + 6.0f) return false;
        }
        return true;
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
    template <typename Fn>
    bool placeFacing(Structure& s, Lot& lot, BuildContext& ctx, Fn&& buildFn, int w, int d, float px, float pz, float nx, float nz, float road_half,
                     int y, const std::function<bool(const Rect&)>& inside) {
        const int rot = rotFacing(-nx, -nz);
        // (el tamano se sabe antes de construir: no se construye lo que no cabe)
        const int sx = (rot & 1) ? d : w, sz = (rot & 1) ? w : d;
        const float depth = std::abs(nx) * static_cast<float>(sx) + std::abs(nz) * static_cast<float>(sz);
        const float dist = road_half + 0.5f + depth * 0.5f;
        const float ccx = px + nx * dist, ccz = pz + nz * dist;
        const int x0 = static_cast<int>(std::lround(ccx - static_cast<float>(sx) * 0.5f));
        const int z0 = static_cast<int>(std::lround(ccz - static_cast<float>(sz) * 0.5f));
        const Rect r{x0, z0, x0 + sx - 1, z0 + sz - 1};
        if (!inside(r) || !lot.freeRect(r, 1) || !lot.clearOfRoads(r, 0.0f)) {
            ctx.points.clear();
            return false;
        }
        lot.used.push_back(r);
        buildFn(ctx);
        add(s, ctx, x0, y, z0, rot);
        return true;
    }

    // --- Aldea ---------------------------------------------------------------
    bool tryVillage() {
        for (int attempt = 0; attempt < 6; ++attempt) {
            const int R = 32 + rng.range(0, 14);
            const int lim = kHalf - R - 30;
            const int cx = cx0 + rng.range(-lim, lim), cz = cz0 + rng.range(-lim, lim);
            if (!clearSpot(cx, cz, static_cast<float>(R + 24))) continue;
            const SiteStats st = site(cx, cz, R, 5);
            if (st.water > 1 || st.max - st.min > 9 || st.min < sea + 1) continue;
            if (st.biome == Generator::AMountains || st.biome == Generator::AOcean || st.biome == Generator::ARiver) continue;
            buildVillage(cx, cz, R, std::max(st.median, sea + 2), styleFor(st.biome));
            return true;
        }
        return false;
    }

    void buildVillage(int cx, int cz, int R, int base, BuildStyle style) {
        Rng nr(hashOf(seed, rx, rz, 31));
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
        tf.decor = style == BuildStyle::Temperate ? 0.08f : 0.025f;
        s.terraforms.push_back(tf);
        static const BlockId kRoofs[] = {B::RoofRed, B::RoofBlue, B::RoofGreen, B::RoofOrange};
        const BlockId main_roof = style == BuildStyle::Snow ? (rng.f() < 0.5f ? B::RoofSlate : B::RoofBlue) : rng.pick(kRoofs);
        const BlockId alt_roof = style == BuildStyle::Snow ? main_roof : rng.pick(kRoofs);
        const auto roof = [&]() { return rng.f() < 0.7f ? main_roof : alt_roof; };

        // Caminos desde la plaza.
        const BlockId path = style == BuildStyle::Desert ? B::Sandstone : B::DirtPath;
        const BlockId path_alt = style == BuildStyle::Desert ? B::Sand : (style == BuildStyle::Snow ? B::Snow : B::Gravel);
        Road plaza{static_cast<float>(cx), static_cast<float>(cz), static_cast<float>(cx), static_cast<float>(cz), 6.5f, path, path_alt, 0.25f};
        s.roads.push_back(plaza);
        const int n_roads = 3 + (rng.f() < 0.5f ? 1 : 0);
        const float a0 = rng.f() * 2.0f * kPi;
        std::vector<std::array<float, 2>> dirs;
        for (int i = 0; i < n_roads; ++i) {
            const float a = a0 + static_cast<float>(i) * 2.0f * kPi / static_cast<float>(n_roads) + (rng.f() - 0.5f) * 0.5f;
            const float dx = std::cos(a), dz = std::sin(a);
            dirs.push_back({dx, dz});
            s.roads.push_back(Road{static_cast<float>(cx), static_cast<float>(cz), static_cast<float>(cx) + dx * static_cast<float>(R + 14),
                                   static_cast<float>(cz) + dz * static_cast<float>(R + 14), 1.6f, path, path_alt, 0.25f});
        }
        Lot lot;
        lot.roads = &s.roads;
        lot.used.push_back(Rect{cx - 7, cz - 7, cx + 7, cz + 7});
        // Pozo en la plaza (el plano empieza 3 bajo el suelo).
        {
            BuildContext ctx = context(style, main_roof, 1);
            build::well(ctx);
            add(s, ctx, cx - 2, base - 3, cz - 2, 0);
        }
        addPoint(s, "plaza", static_cast<float>(cx) + 0.5f, static_cast<float>(base + 1), static_cast<float>(cz) + 3.5f);
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
        std::size_t next = 0;
        int placed = 0;
        for (int t = 10; t < R; t += 5) {
            for (std::size_t ri = 0; ri < dirs.size(); ++ri) {
                for (int side = -1; side <= 1; side += 2) {
                    const float dx = dirs[ri][0], dz = dirs[ri][1];
                    const float px = static_cast<float>(cx) + dx * static_cast<float>(t), pz = static_cast<float>(cz) + dz * static_cast<float>(t);
                    const float nx = -dz * static_cast<float>(side), nz = dx * static_cast<float>(side);
                    const Kind kind = next < queue.size() ? queue[next] : KHouse;
                    BuildContext ctx = context(style, roof(), 10 + placed);
                    const int hw = rng.range(7, 9), hd = rng.range(7, 8);
                    const int floors = style == BuildStyle::Desert ? rng.range(1, 2) : (rng.f() < 0.45f ? 2 : 1);
                    std::string tag;
                    switch (style) {
                        case BuildStyle::Snow: tag = rng.pick(kSnowTags); break;
                        case BuildStyle::Desert: tag = rng.pick(kDesertTags); break;
                        case BuildStyle::Beach: tag = rng.pick(kBeachTags); break;
                        default: tag = rng.pick(kTags); break;
                    }
                    const auto fn = [&](BuildContext& c) {
                        switch (kind) {
                            case KTavern: build::tavern(c); break;
                            case KSmithy: build::smithy(c, false); break;
                            case KShopGeneral: build::shop(c, "general"); break;
                            case KShopPotions: build::shop(c, "pociones"); break;
                            default: build::house(c, hw, hd, floors, tag); break;
                        }
                    };
                    const int bw = kind == KTavern ? 15 : (kind == KSmithy ? 12 : (kind == KHouse ? hw + 2 : 11));
                    const int bd = kind == KTavern ? 13 : (kind == KSmithy ? 10 : (kind == KHouse ? hd + 2 : 10));
                    if (placeFacing(s, lot, ctx, fn, bw, bd, px, pz, nx, nz, 1.6f, base, inside)) {
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
                const int x = static_cast<int>(std::lround(static_cast<float>(cx) + dx * static_cast<float>(t) - dz * side * 2.8f));
                const int z = static_cast<int>(std::lround(static_cast<float>(cz) + dz * static_cast<float>(t) + dx * side * 2.8f));
                const Rect r{x, z, x, z};
                if (!lot.freeRect(r, 0) || !lot.clearOfRoads(r, 0.0f)) continue;
                lot.used.push_back(r);
                BuildContext ctx = context(style, main_roof, 500 + t);
                build::lampPost(ctx);
                add(s, ctx, x, base, z, 0);
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
            add(s, ctx, x0, base, z0, 0);
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
            if (style == BuildStyle::Desert) {
                build::stall(ctx, B::ClothYellow, "general");
                add(s, ctx, x0 + 1, base, z0 + 2, rng.range(0, 3));
            } else {
                build::tree(ctx);
                add(s, ctx, x0, base, z0, 0);
            }
            ++made;
        }
        for (int i = 0; i < 2; ++i) {
            const int x = cx + (i ? 4 : -5), z = cz + (i ? -5 : 4);
            BuildContext ctx = context(style, main_roof, 800 + i);
            build::bench(ctx);
            add(s, ctx, x, base, z, i);
        }
        finish(s);
    }

    // --- Ciudad (y capital) --------------------------------------------------------
    bool tryCity() {
        for (int attempt = 0; attempt < 6; ++attempt) {
            const int H = 54 + rng.range(0, 8);
            const int lim = kHalf - H - 40;
            if (lim < 0) return false;
            const int cx = cx0 + rng.range(-lim, lim), cz = cz0 + rng.range(-lim, lim);
            if (!clearSpot(cx, cz, static_cast<float>(H + 36))) continue;
            const SiteStats st = site(cx, cz, H, 7);
            if (st.water > 2 || st.max - st.min > 16 || st.biome == Generator::AMountains) continue;
            Rng nr(hashOf(seed, rx, rz, 37));
            buildCity(cx, cz, H, std::max(st.median, sea + 3), cityName(nr), false, styleFor(st.biome));
            return true;
        }
        return false;
    }

    void buildCapital() {
        // El mejor sitio cerca del origen (o el menos malo: se aplana lo que haga falta).
        const int H = 84;
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
        buildCity(best_x, best_z, H, best_base, "Kettlecrown", true, BuildStyle::City);
    }

    void buildCity(int cx, int cz, int H, int base, const std::string& name, bool capital, BuildStyle biome_style) {
        Structure& s = begin("ciudad", name, cx, base + 1, cz, capital ? 1 : levelAt(cx, cz, 0.5f));
        s.clear_radius = static_cast<float>(H) * 1.42f + 30.0f;
        const BuildStyle style = BuildStyle::City;
        Terraform tf;
        tf.cx = cx;
        tf.cz = cz;
        tf.inner = static_cast<float>(H + 8);
        tf.outer = static_cast<float>(H + 30);
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
        const int P = capital ? 16 : 12;  // radio de la plaza
        const int castle_w = capital ? 45 : 35, castle_d = capital ? 40 : 30;  // ancho impar: puerta centrada
        const int castle_z0 = cz - H + 4;
        const int castle_z1 = castle_z0 + castle_d - 1;
        const float fcx = static_cast<float>(cx), fcz = static_cast<float>(cz);
        // Calles: principales (empedradas) en cruz, anillo junto a la muralla y secundarias.
        const auto street = [&](float x0, float z0, float x1, float z1, float half, BlockId surf, BlockId alt) {
            s.roads.push_back(Road{x0, z0, x1, z1, half, surf, alt, 0.12f});
        };
        street(fcx, fcz, fcx, fcz, static_cast<float>(P), B::Paving, B::StoneBricks);  // plaza redonda
        street(fcx, static_cast<float>(castle_z1 + 1), fcx, fcz + static_cast<float>(H + 14), 3.0f, B::Paving, B::Cobblestone);
        street(fcx - static_cast<float>(H + 14), fcz, fcx + static_cast<float>(H + 14), fcz, 3.0f, B::Paving, B::Cobblestone);
        const float ring = static_cast<float>(H - 8);
        const float sec = static_cast<float>(H) * 0.5f;
        street(fcx - ring, fcz + ring, fcx + ring, fcz + ring, 2.0f, B::Cobblestone, B::Gravel);
        street(fcx - ring, fcz - ring, fcx - ring, fcz + ring, 2.0f, B::Cobblestone, B::Gravel);
        street(fcx + ring, fcz - ring, fcx + ring, fcz + ring, 2.0f, B::Cobblestone, B::Gravel);
        street(fcx - ring, static_cast<float>(castle_z1 + 3), fcx + ring, static_cast<float>(castle_z1 + 3), 2.0f, B::Cobblestone, B::Gravel);
        street(fcx - sec, static_cast<float>(castle_z1 + 3), fcx - sec, fcz + ring, 2.0f, B::Cobblestone, B::Gravel);
        street(fcx + sec, static_cast<float>(castle_z1 + 3), fcx + sec, fcz + ring, 2.0f, B::Cobblestone, B::Gravel);
        street(fcx - ring, fcz + sec, fcx + ring, fcz + sec, 2.0f, B::Cobblestone, B::Gravel);
        if (fcz - sec > static_cast<float>(castle_z1 + 6)) street(fcx - ring, fcz - sec, fcx + ring, fcz - sec, 2.0f, B::Cobblestone, B::Gravel);

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
            const int ts = 9;
            std::vector<std::array<int, 3>> towers = {{-H + 1, -H + 1, 2}, {H - 2, -H + 1, 2}, {-H + 1, H - 2, 0}, {H - 2, H - 2, 0}};
            if (capital) {
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
                build::tower(ctx, (t[0] == -H + 1 || t[0] == H - 2) && (t[1] == -H + 1 || t[1] == H - 2) ? ts : 7, wall_h + 6);
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
            const int fs = capital ? 11 : 9;
            BuildContext ctx = context(style, main_roof, 1400);
            build::fountain(ctx, fs);
            add(s, ctx, cx - fs / 2, base - 1, cz - fs / 2, 0);
            lot.used.push_back(Rect{cx - P, cz - P, cx + P, cz + P});
            addPoint(s, "plaza", fcx + 0.5f, static_cast<float>(base + 1), fcz + static_cast<float>(fs / 2 + 2) + 0.5f);
            if (capital) addPoint(s, "inicio", fcx + 0.5f, static_cast<float>(base + 1), fcz + static_cast<float>(fs / 2 + 4) + 0.5f, "", 0);
            static const char* const kShops[] = {"armas", "armaduras", "pociones", "monturas", "pociones", "armas"};
            static const BlockId kCloth[] = {B::ClothRed, B::ClothBlue, B::ClothYellow, B::ClothRed, B::ClothBlue, B::ClothYellow};
            const int stalls = capital ? 12 : 6;
            int made = 0;
            for (int i = 0; i < 24 && made < stalls; ++i) {
                const float a = (static_cast<float>(i) + 0.5f) * 2.0f * kPi / 24.0f;
                const float dx = std::cos(a), dz = std::sin(a);
                if (std::abs(dx) > 0.92f || std::abs(dz) > 0.92f) continue;  // no en las calles
                const float rr = static_cast<float>(P) - 3.5f;
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
                build::lampPost(lc);
                add(s, lc, x, base, z, 0);
                lot.used.push_back(Rect{x, z, x, z});
            }
            for (int i = 0; i < 4; ++i) {
                BuildContext bc = context(style, main_roof, 1650 + i);
                build::bench(bc);
                const float a = static_cast<float>(i) * kPi * 0.5f + kPi * 0.25f;
                const float br = static_cast<float>(fs / 2 + 4);
                add(s, bc, cx + static_cast<int>(std::lround(std::cos(a) * br)), base, cz + static_cast<int>(std::lround(std::sin(a) * br)), i);
            }
        }
        // --- Edificios a lo largo de las calles ---
        const int lim = H - 6;
        const auto inside = [&](const Rect& r) {
            return r.x0 >= cx - lim && r.x1 <= cx + lim && r.z0 >= cz - lim && r.z1 <= cz + lim;
        };
        enum Kind { KTavern, KSmithy, KTemple, KPotions, KArmor, KGeneral, KStable, KHouse };
        std::vector<Kind> queue = {KTavern, KTemple, KSmithy, KPotions, KArmor, KGeneral, KStable};
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
        std::vector<Walk> arms = {{fcx, fcz + static_cast<float>(P), fcx, fcz + static_cast<float>(H), 3.0f},
                                  {fcx + static_cast<float>(P), fcz, fcx + static_cast<float>(H), fcz, 3.0f},
                                  {fcx - static_cast<float>(P), fcz, fcx - static_cast<float>(H), fcz, 3.0f},
                                  {fcx, fcz - static_cast<float>(P), fcx, static_cast<float>(castle_z1 + 2), 3.0f}};
        const auto tryAlong = [&](const Walk& wk, float t, int side) {
            const float len = std::hypot(wk.x1 - wk.x0, wk.z1 - wk.z0);
            if (t > len) return;
            const float dx = (wk.x1 - wk.x0) / len, dz = (wk.z1 - wk.z0) / len;
            const float px = wk.x0 + dx * t, pz = wk.z0 + dz * t;
            const float nx = -dz * static_cast<float>(side), nz = dx * static_cast<float>(side);
            const Kind kind = next < queue.size() ? queue[next] : KHouse;
            BuildContext ctx = context(style, roof(), 2000 + placed * 3 + side);
            const int hw = rng.range(7, 10), hd = rng.range(7, 9);
            const int floors = capital ? rng.range(2, 3) : rng.range(1, 3);
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
                    const int x = static_cast<int>(std::lround(a.x0 + dx * t - dz * static_cast<float>(side) * 3.6f));
                    const int z = static_cast<int>(std::lround(a.z0 + dz * t + dx * static_cast<float>(side) * 3.6f));
                    const Rect r{x, z, x, z};
                    if (!lot.freeRect(r, 0)) continue;
                    lot.used.push_back(r);
                    BuildContext lc = context(style, main_roof, 3000 + static_cast<int>(t) * 2 + side);
                    build::lampPost(lc);
                    add(s, lc, x, base, z, 0);
                }
            }
        }
        for (int i = 0, made = 0; i < 160 && made < (capital ? 26 : 12); ++i) {
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
        finish(s);
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
        Rng r(hashOf(seed, rx * 31 + salt, rz, level));
        if (level >= 6 && r.f() < 0.4f) return r.pick(kHigh);
        switch (biome) {
            case Generator::AForest: return r.pick(kForest);
            case Generator::ADesert: return r.pick(kDesert);
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
    Planner pl(gen_, dimension_, world_seed, rx, rz, *plan);
    if (rx == 0 && rz == 0) {
        pl.buildCapital();  // siempre la capital en el origen
        return plan;
    }
    const float r = pl.rng.f();
    bool settled = false;
    if (r < 0.08f) settled = pl.tryCity();
    if (!settled && r < 0.55f) settled = pl.tryVillage();
    if (pl.rng.f() < 0.55f) pl.tryDungeon();
    if (pl.rng.f() < 0.18f) pl.tryTower();
    if (pl.rng.f() < 0.35f) pl.tryRuin();
    if (pl.rng.f() < 0.07f) pl.tryPortal();
    return plan;
}

// --- Estampado ----------------------------------------------------------------------------

void Structures::stamp(ChunkData& chunk) const {
    const int x0 = chunk.cx * kChunkSize, z0 = chunk.cz * kChunkSize;
    const int x1 = x0 + kChunkSize - 1, z1 = z0 + kChunkSize - 1;
    const std::shared_ptr<const RegionPlan> plan = region(regionOf(x0), regionOf(z0));
    std::vector<const Structure*> touching;
    for (const Structure& s : plan->structures) {
        float reach = 0.0f;
        for (const Terraform& tf : s.terraforms) reach = std::max(reach, tf.outer + 2.0f);
        const int mx0 = std::min(s.min_x, static_cast<int>(s.clear_x - reach)), mx1 = std::max(s.max_x, static_cast<int>(s.clear_x + reach));
        const int mz0 = std::min(s.min_z, static_cast<int>(s.clear_z - reach)), mz1 = std::max(s.max_z, static_cast<int>(s.clear_z + reach));
        if (mx1 < x0 || mx0 > x1 || mz1 < z0 || mz0 > z1) continue;
        touching.push_back(&s);
    }
    if (touching.empty()) return;
    const auto at = [&](int x, int y, int z) -> BlockId& { return chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))]; };
    const int sea = gen_.seaLevel();
    std::array<int, kChunkSize * kChunkSize> top{};
    top.fill(-1);
    const auto findTop = [&](int lx, int lz) {
        int& t = top[static_cast<std::size_t>(lx + lz * kChunkSize)];
        if (t >= 0) return t;
        for (int y = kWorldHeight - 1; y > 0; --y) {
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
                    const int target = std::clamp(static_cast<int>(std::lround(static_cast<float>(natural) + (static_cast<float>(tf.base) - static_cast<float>(natural)) * w)), 2, kWorldHeight - 40);
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
    // 2. Calles y caminos sobre el suelo.
    for (const Structure* s : touching) {
        for (const Road& road : s->roads) {
            const float minx = std::min(road.x0, road.x1) - road.half_width - 1.0f, maxx = std::max(road.x0, road.x1) + road.half_width + 1.0f;
            const float minz = std::min(road.z0, road.z1) - road.half_width - 1.0f, maxz = std::max(road.z0, road.z1) + road.half_width + 1.0f;
            if (maxx < static_cast<float>(x0) || minx > static_cast<float>(x1 + 1) || maxz < static_cast<float>(z0) || minz > static_cast<float>(z1 + 1)) continue;
            for (int lz = 0; lz < kChunkSize; ++lz) {
                for (int lx = 0; lx < kChunkSize; ++lx) {
                    const float px = static_cast<float>(x0 + lx) + 0.5f, pz = static_cast<float>(z0 + lz) + 0.5f;
                    const float d = roadDistance(road, px, pz);
                    if (d > road.half_width) continue;
                    const int t = findTop(lx, lz);
                    if (t < sea - 1 || t <= 1) continue;  // sin puentes
                    const std::uint32_t h = hashOf(0xA0Du, x0 + lx, z0 + lz, t);
                    const float r = static_cast<float>(h & 0xFFFF) / 65536.0f;
                    // Borde irregular en los caminos de tierra.
                    if (road.surface == B::DirtPath && d > road.half_width - 0.6f && r < 0.5f) continue;
                    at(lx, t, lz) = (road.alt != B::Air && r < road.alt_chance) ? road.alt : road.surface;
                    for (int y = t + 1; y <= t + 2 && y < kWorldHeight; ++y) {
                        const BlockId b = at(lx, y, lz);
                        if (b != B::Air && blockDef(b).shape == BlockShape::Cross) at(lx, y, lz) = B::Air;
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
        for (const Structure& s : plan->structures) {
            const float dx = static_cast<float>(x) - s.clear_x, dz = static_cast<float>(z) - s.clear_z;
            if (dx * dx + dz * dz < s.clear_radius * s.clear_radius) return true;
        }
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
            for (const Structure& s : plan->structures) {
                const float dx = px - s.clear_x, dz = pz - s.clear_z;
                const float r = s.clear_radius + margin;
                if (dx * dx + dz * dz < r * r) return true;
                for (const Road& road : s.roads) {
                    if (roadDistance(road, px, pz) < road.half_width + margin) return true;
                }
            }
        }
    }
    return false;
}

int Structures::groundHeight(int x, int z, int natural) const {
    const std::shared_ptr<const RegionPlan> plan = region(regionOf(x), regionOf(z));
    int h = natural;
    for (const Structure& s : plan->structures) {
        for (const Terraform& tf : s.terraforms) {
            const float w = terraformWeight(tf, x, z);
            if (w <= 0.0f) continue;
            h = static_cast<int>(std::lround(static_cast<float>(h) + (static_cast<float>(tf.base) - static_cast<float>(h)) * w));
        }
    }
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
            for (const Structure& s : plan->structures) {
                const float qx = std::clamp(x, s.info.min.x, s.info.max.x);
                const float qy = std::clamp(y, s.info.min.y, s.info.max.y);
                const float qz = std::clamp(z, s.info.min.z, s.info.max.z);
                const float d2 = (x - qx) * (x - qx) + (y - qy) * (y - qy) + (z - qz) * (z - qz);
                if (d2 <= radius * radius) out.push_back(s.info);
            }
        }
    }
    return out;
}

bool Structures::at(float x, float y, float z, StructureInfo& out) const {
    const std::shared_ptr<const RegionPlan> plan = region(regionOf(static_cast<int>(std::floor(x))), regionOf(static_cast<int>(std::floor(z))));
    const Structure* best = nullptr;
    int best_room = -1;
    float best_volume = 1e30f;
    for (const Structure& s : plan->structures) {
        if (x < s.info.min.x || x >= s.info.max.x || y < s.info.min.y || y >= s.info.max.y || z < s.info.min.z || z >= s.info.max.z) continue;
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
    }
    if (best == nullptr) return false;
    out = best->info;
    out.room = best->info.type == "mazmorra" ? best_room : -1;
    return true;
}

}  // namespace cramion::voxel
