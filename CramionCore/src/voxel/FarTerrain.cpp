// Terreno lejano de los mundos de bloques (estilo "Distant Horizons"). Ver
// FarTerrain.h: teselas en arbol cuaternario, columnas del generador, mallas
// en los hilos de jobs, cache en disco y cambio sin agujeros.

#include "FarTerrain.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/jobs/JobSystem.h"

#include <CramionFX/vk/VoxelPass.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace cramion::voxel {

namespace {

using Clock = std::chrono::steady_clock;
using core::Vec3;

cvar::CVar<int> g_far_distance("voxel.far.Distance", 0,
                               "Terreno lejano de los mundos de bloques (estilo Distant Horizons): hasta donde se ve, en m "
                               "(0 = apagado). Mas alla de render_distance, mallas simplificadas del generador.",
                               cvar::None, 0, 32768);
cvar::CVar<float> g_far_detail("voxel.far.Detail", 1.0f,
                               "Detalle del terreno lejano: 1 normal, 2 el doble de columnas a la misma distancia (4x teselas)",
                               cvar::None, 0.25f, 4.0f);
cvar::CVar<bool> g_far_cache("voxel.far.DiskCache", true,
                             "Guardar las columnas del terreno lejano en disco (%LOCALAPPDATA%/Cramion/FarTerrain)");
cvar::CVar<bool> g_far_sea("voxel.far.Sea", true,
                           "Terreno lejano: dibujar la superficie del mar (apagado: el fondo, si la escena tiene su propio oceano)");

constexpr int kMinLevel = 0;
constexpr int kFillLevel = 3;   // dentro de los chunks de verdad: teselas de 8 m por columna como mucho
constexpr int kRootLevel = 10;  // teselas de 16 km
constexpr std::uint32_t kCacheMagic = 0x52414643u;  // "CFAR"
constexpr std::uint32_t kCacheVersion = 7;          // subir si cambia Generator::farColumn (2: tierras salvajes e islas; 3: oceanos hondos; 4: biomas frondosos, NOCHE-MUNDO; 5: NATURALEZA-HZ; 6: OCEANOS-HZ; 7: IDS16, bloques de 16 bits)

std::uint64_t tileKey(int level, int tx, int tz) {
    const auto ux = static_cast<std::uint64_t>(static_cast<std::uint32_t>(tx + (1 << 25)) & 0x3FFFFFFu);
    const auto uz = static_cast<std::uint64_t>(static_cast<std::uint32_t>(tz + (1 << 25)) & 0x3FFFFFFu);
    return (static_cast<std::uint64_t>(level) << 52) | (ux << 26) | uz;
}

int floorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }

// --- Cache en disco --------------------------------------------------------------

#pragma pack(push, 1)
struct PackedColumn {
    std::uint8_t height, water, canopy, flags;  // flags: 1 mar, 2 acantilado posible
    BlockId top, under, deep, leaves;           // IDS16: bloques de 16 bits (cache v7)
    std::uint16_t grass_tint, foliage_tint;
    std::uint8_t island_bottom, island_top;     // isla flotante (top 0 = ninguna)
    BlockId island_block, island_rock;
};
#pragma pack(pop)
static_assert(sizeof(PackedColumn) == 22);

std::filesystem::path cacheRoot() {
#ifdef _WIN32
    char* local = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&local, &length, "LOCALAPPDATA") == 0 && local != nullptr) {
        const std::string value(local);
        std::free(local);
        if (!value.empty()) return std::filesystem::path(value) / "Cramion" / "FarTerrain";
    }
#else
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".cache" / "Cramion" / "FarTerrain";
    }
#endif
    std::error_code error;
    return std::filesystem::temp_directory_path(error) / "Cramion" / "FarTerrain";
}

std::filesystem::path cacheFolder(const Generator& g) {
    std::ostringstream name;
    name << "v" << kCacheVersion << "_g" << static_cast<int>(g.profile()) << "_s" << g.seed() << "_sea" << g.seaLevel() << "_t"
         << std::lround(g.treeDensity() * 1000.0f) << "_c" << std::lround(g.clearings() * 1000.0f);
    return cacheRoot() / name.str();
}

std::filesystem::path cacheFile(const std::filesystem::path& folder, int level, int tx, int tz) {
    return folder / ("L" + std::to_string(level)) / (std::to_string(tx) + "_" + std::to_string(tz) + ".far");
}

bool readCache(const std::filesystem::path& file, int level, int tx, int tz, std::vector<Generator::FarColumn>& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::uint32_t header[5]{};
    if (!in.read(reinterpret_cast<char*>(header), sizeof(header))) return false;
    if (header[0] != kCacheMagic || header[1] != kCacheVersion || header[2] != static_cast<std::uint32_t>(level) ||
        header[3] != static_cast<std::uint32_t>(tx) || header[4] != static_cast<std::uint32_t>(tz)) {
        return false;
    }
    constexpr std::size_t kCount = FarTerrain::kBorder * FarTerrain::kBorder;
    std::vector<PackedColumn> packed(kCount);
    if (!in.read(reinterpret_cast<char*>(packed.data()), static_cast<std::streamsize>(kCount * sizeof(PackedColumn)))) return false;
    out.resize(kCount);
    const auto valid = [](BlockId b) { return b < block::Count ? b : block::Air; };  // IDS16: nunca fuera de la paleta
    for (std::size_t i = 0; i < kCount; ++i) {
        const PackedColumn& p = packed[i];
        Generator::FarColumn& c = out[i];
        c.height = p.height;
        c.top = valid(p.top);
        c.under = valid(p.under);
        c.deep = valid(p.deep);
        c.water = p.water;
        c.canopy = p.canopy;
        c.leaves = valid(p.leaves);
        c.sea = (p.flags & 1u) != 0;
        c.cliffable = (p.flags & 2u) != 0;
        c.grass_tint = p.grass_tint;
        c.foliage_tint = p.foliage_tint;
        c.island_bottom = p.island_bottom;
        c.island_top = p.island_top;
        c.island_block = valid(p.island_block);
        c.island_rock = valid(p.island_rock);
    }
    return true;
}

void writeCache(const std::filesystem::path& file, int level, int tx, int tz, const std::vector<Generator::FarColumn>& columns) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    // Nombre temporal unico (dos tareas de la misma tesela no se pisan) y
    // rename: nunca queda un archivo a medias.
    std::ostringstream suffix;
    suffix << ".tmp" << std::hash<std::thread::id>{}(std::this_thread::get_id()) << "_" << Clock::now().time_since_epoch().count();
    const std::filesystem::path temp = file.string() + suffix.str();
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        const std::uint32_t header[5] = {kCacheMagic, kCacheVersion, static_cast<std::uint32_t>(level), static_cast<std::uint32_t>(tx),
                                         static_cast<std::uint32_t>(tz)};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        std::vector<PackedColumn> packed(columns.size());
        for (std::size_t i = 0; i < columns.size(); ++i) {
            const Generator::FarColumn& c = columns[i];
            packed[i] = PackedColumn{static_cast<std::uint8_t>(std::clamp(c.height, 0, 255)),
                                     static_cast<std::uint8_t>(std::clamp(c.water, 0, 255)),
                                     static_cast<std::uint8_t>(std::clamp(c.canopy, 0, 255)),
                                     static_cast<std::uint8_t>((c.sea ? 1u : 0u) | (c.cliffable ? 2u : 0u)),
                                     c.top, c.under, c.deep, c.leaves, c.grass_tint, c.foliage_tint,
                                     static_cast<std::uint8_t>(std::clamp(c.island_bottom, 0, 255)),
                                     static_cast<std::uint8_t>(std::clamp(c.island_top, 0, 255)), c.island_block, c.island_rock};
        }
        out.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size() * sizeof(PackedColumn)));
        if (!out) {
            out.close();
            std::filesystem::remove(temp, error);
            return;
        }
    }
    std::filesystem::rename(temp, file, error);
    if (error) std::filesystem::remove(temp, error);
}

// --- Malla ------------------------------------------------------------------------

// Vertice lejano (voxel.vert con VoxelPass::setFarSection):
//   a: x 0..16 (5) | y bits 0..4 (5) | z 0..16 (5) | cara (3) | u (1) | v (1) | oclusion (2) | 0 | y bits 5..8 (4)
//   b: R G B sRGB (8 + 8 + 8) | rugosidad 0..15 (4) | agua (1)
// x y z en columnas (el VoxelPass las escala por 2^nivel); y en bloques (0..511).
std::uint32_t packFarA(int x, int y, int z, int face, int u, int v, int ao) {
    return static_cast<std::uint32_t>(x) | (static_cast<std::uint32_t>(y & 31) << 5) | (static_cast<std::uint32_t>(z) << 10) |
           (static_cast<std::uint32_t>(face) << 15) | (static_cast<std::uint32_t>(u) << 18) | (static_cast<std::uint32_t>(v) << 19) |
           (static_cast<std::uint32_t>(ao & 3) << 20) | (static_cast<std::uint32_t>((y >> 5) & 15) << 23);
}

struct Color {
    std::uint8_t r = 0, g = 0, b = 0, roughness = 13;
    bool water = false;
};

std::uint32_t packFarB(const Color& c) {
    return static_cast<std::uint32_t>(c.r) | (static_cast<std::uint32_t>(c.g) << 8) | (static_cast<std::uint32_t>(c.b) << 16) |
           (static_cast<std::uint32_t>(c.roughness & 15) << 24) | (c.water ? (1u << 28) : 0u);
}

Color tinted(const FarPalette::Entry& e, std::uint16_t tint) {
    const auto k = [](std::uint8_t v, int t) { return static_cast<std::uint8_t>((static_cast<int>(v) * t + 7) / 15); };
    return Color{k(e.r, tint & 15), k(e.g, (tint >> 4) & 15), k(e.b, (tint >> 8) & 15), e.roughness};
}

// Una cara de la caja [lo, hi] (columnas en x y z, bloques en y), como
// Mesher.cpp emitBox: esquinas abajo-izquierda, abajo-derecha, arriba-derecha,
// arriba-izquierda (antihorario visto desde fuera). `upper` / `lower`: color y
// oclusion de las esquinas de arriba y de abajo (caras laterales).
void emitFace(std::vector<std::uint32_t>& out, int face, const int lo[3], const int hi[3], const Color& upper, const Color& lower,
              int ao_upper, int ao_lower) {
    const FaceAxes& axes = kFaces[static_cast<std::size_t>(face)];
    static constexpr int kCorner[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (int c = 0; c < 4; ++c) {
        const int su = kCorner[c][0], sv = kCorner[c][1];
        int p[3];
        for (int k = 0; k < 3; ++k) {
            bool high;
            if (axes.n[k] != 0) high = axes.n[k] > 0;
            else if (axes.u[k] != 0) high = axes.u[k] * su > 0;
            else high = axes.v[k] * sv > 0;
            p[k] = high ? hi[k] : lo[k];
        }
        // En las caras laterales v va hacia abajo: sv > 0 es la esquina de abajo.
        const bool bottom = face != 2 && face != 3 && sv > 0;
        out.push_back(packFarA(p[0], p[1], p[2], face, su > 0 ? 1 : 0, sv > 0 ? 1 : 0, bottom ? ao_lower : ao_upper));
        out.push_back(packFarB(bottom ? lower : upper));
    }
}

}  // namespace

// --- Paleta -------------------------------------------------------------------------

std::shared_ptr<FarPalette> buildFarPalette(const std::vector<gfx::VoxelTextureLayer>& layers) {
    auto palette = std::make_shared<FarPalette>();
    std::array<float, 256> to_linear{};
    for (int i = 0; i < 256; ++i) to_linear[static_cast<std::size_t>(i)] = std::pow(static_cast<float>(i) / 255.0f, 2.2f);
    struct Average {
        bool valid = false;
        FarPalette::Entry entry;
    };
    std::vector<Average> averages(layers.size());
    for (std::size_t l = 0; l < layers.size(); ++l) {
        const auto& albedo = layers[l].albedo;
        const auto& material = layers[l].material;
        double r = 0.0, g = 0.0, b = 0.0, n = 0.0, rough = 0.0, rn = 0.0;
        for (std::size_t i = 0; i + 3 < albedo.size(); i += 4) {
            if (albedo[i + 3] < 128) continue;  // huecos (hojas, cristal, plantas)
            r += to_linear[albedo[i]];
            g += to_linear[albedo[i + 1]];
            b += to_linear[albedo[i + 2]];
            n += 1.0;
            if (i < material.size()) {
                rough += material[i];
                rn += 1.0;
            }
        }
        if (n <= 0.0) continue;
        const auto srgb = [](double v) {
            return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(std::pow(v, 1.0 / 2.2) * 255.0)), 0, 255));
        };
        Average& a = averages[l];
        a.valid = true;
        a.entry.r = srgb(r / n);
        a.entry.g = srgb(g / n);
        a.entry.b = srgb(b / n);
        a.entry.roughness = rn > 0.0 ? static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(rough / rn / 255.0 * 15.0)), 1, 15))
                                     : std::uint8_t{13};
    }
    const auto entry = [&](const std::string& name, const FarPalette::Entry& fallback) {
        if (name.empty()) return fallback;
        const int layer = textureLayer(name);
        if (layer < 0 || static_cast<std::size_t>(layer) >= averages.size() || !averages[static_cast<std::size_t>(layer)].valid) {
            return fallback;
        }
        return averages[static_cast<std::size_t>(layer)].entry;
    };
    for (int id = 0; id < block::Count; ++id) {
        const BlockDef& def = blockDef(static_cast<BlockId>(id));
        const FarPalette::Entry top = entry(def.top, FarPalette::Entry{});
        palette->top[static_cast<std::size_t>(id)] = top;
        palette->side[static_cast<std::size_t>(id)] = entry(def.side, top);
    }
    // El agua: su textura es casi blanca (el color lo pone el pase del agua);
    // de lejos, el azul medio del agua del motor (entre el claro y el hondo).
    palette->top[block::Water] = palette->side[block::Water] = FarPalette::Entry{52, 110, 128, 1};
    return palette;
}

// --- Teselas ------------------------------------------------------------------------

void FarTerrain::sampleTile(const Generator& generator, int level, int tx, int tz, std::vector<Generator::FarColumn>& out) {
    out.assign(static_cast<std::size_t>(kBorder * kBorder), Generator::FarColumn{});
    const int step = 1 << level;
    const int x0 = tx * kSamples * step + step / 2, z0 = tz * kSamples * step + step / 2;  // el centro de cada columna
    const bool detailed = level <= 3;  // aldeas y claros: solo lo que se ve a menos de ~1 km
    for (int z = -1; z <= kSamples; ++z) {
        for (int x = -1; x <= kSamples; ++x) {
            generator.farColumn(x0 + x * step, z0 + z * step, out[static_cast<std::size_t>((x + 1) + (z + 1) * kBorder)], detailed);
        }
    }
}

void FarTerrain::meshTile(const std::vector<Generator::FarColumn>& columns, int level, const FarPalette& palette, bool sea,
                          std::vector<std::uint32_t>& out) {
    out.clear();
    if (columns.size() < static_cast<std::size_t>(kBorder * kBorder)) return;
    const int step = 1 << level;
    struct Info {
        int ground = 0;  // y de la cara de arriba del suelo
        int water = 0;   // y de la superficie del agua (0 = sin agua)
        int canopy = 0;  // y de arriba de la copa (0 = sin arbol)
        int top = 0;     // lo mas alto
    };
    const auto info = [&](int x, int z) {
        const Generator::FarColumn& c = columns[static_cast<std::size_t>((x + 1) + (z + 1) * kBorder)];
        Info i;
        i.ground = std::clamp(c.height + 1, 0, 300);
        if (c.water > c.height && (sea || !c.sea)) i.water = c.water;
        if (i.water == 0 && c.canopy > c.height) i.canopy = c.canopy + 1;
        i.top = std::max({i.ground, i.water, i.canopy});
        return i;
    };
    std::array<Info, kBorder * kBorder> infos{};
    for (int z = -1; z <= kSamples; ++z) {
        for (int x = -1; x <= kSamples; ++x) infos[static_cast<std::size_t>((x + 1) + (z + 1) * kBorder)] = info(x, z);
    }
    const auto at = [&](int x, int z) -> const Info& { return infos[static_cast<std::size_t>((x + 1) + (z + 1) * kBorder)]; };
    const FarPalette::Entry& water_entry = palette.top[block::Water];
    const Color water_color{water_entry.r, water_entry.g, water_entry.b, 2, true};
    // El agua honda, mas oscura y azul (como la absorcion del pase del agua de cerca).
    const auto water_of = [&](const Info& i) {
        const float k = std::clamp(static_cast<float>(i.water - i.ground - 4) / 40.0f, 0.0f, 1.0f);
        if (k <= 0.0f) return water_color;
        const float t = k * k * (3.0f - 2.0f * k);
        const auto dim = [&](std::uint8_t v, float f) { return static_cast<std::uint8_t>(std::lround(static_cast<float>(v) * (1.0f - f * t))); };
        return Color{dim(water_color.r, 0.55f), dim(water_color.g, 0.42f), dim(water_color.b, 0.22f), water_color.roughness, true};
    };
    // Faldon en los bordes: tapa las grietas con las teselas de otro nivel.
    const int skirt = 2 * step + 3;
    const int cliff_slope = std::max(4, static_cast<int>(std::lround(1.2f * static_cast<float>(step))));
    out.reserve(static_cast<std::size_t>(kSamples * kSamples) * 8 * 3);

    static constexpr int kSides[4][3] = {{0, 1, 0}, {1, -1, 0}, {4, 0, 1}, {5, 0, -1}};  // cara, dx, dz
    for (int z = 0; z < kSamples; ++z) {
        for (int x = 0; x < kSamples; ++x) {
            const Generator::FarColumn& c = columns[static_cast<std::size_t>((x + 1) + (z + 1) * kBorder)];
            const Info& me = at(x, z);
            const Color my_water = me.water != 0 ? water_of(me) : water_color;
            // Bloque de arriba: en las laderas empinadas, la roca.
            int slope = 0;
            for (const auto& s : kSides) slope = std::max(slope, std::abs(at(x + s[1], z + s[2]).ground - me.ground));
            const BlockId top_block = c.cliffable && slope >= cliff_slope ? c.deep : c.top;
            const BlockDef& top_def = blockDef(top_block);
            const Color ground_top = top_def.tint == BlockTint::Grass   ? tinted(palette.top[top_block], c.grass_tint)
                                     : top_def.tint == BlockTint::Foliage ? tinted(palette.top[top_block], c.foliage_tint)
                                                                         : tinted(palette.top[top_block], 0xFFF);
            const Color leaves = tinted(palette.top[c.leaves], blockDef(c.leaves).tint != BlockTint::None ? c.foliage_tint : 0xFFF);
            const Color under = tinted(palette.side[c.under], 0xFFF);
            const Color deep = tinted(palette.side[c.deep], 0xFFF);

            // Arriba.
            {
                const Color& color = me.water != 0 ? my_water : (me.canopy != 0 ? leaves : ground_top);
                const int lo[3] = {x, me.top - 1, z}, hi[3] = {x + 1, me.top, z + 1};
                emitFace(out, 2, lo, hi, color, color, 3, 3);
            }
            // Lados: hasta el vecino mas bajo (y el faldon en el borde de la tesela).
            for (const auto& s : kSides) {
                const int nx = x + s[1], nz = z + s[2];
                const bool border = nx < 0 || nz < 0 || nx >= kSamples || nz >= kSamples;
                int bottom = at(nx, nz).top;
                if (border) bottom = std::min(bottom, me.top - skirt);
                bottom = std::max(bottom, 0);
                if (bottom >= me.top) continue;
                const auto side = [&](int y0, int y1, const Color& upper, const Color& lower, int ao_lower) {
                    if (y0 >= y1) return;
                    const int lo[3] = {x, y0, z}, hi[3] = {x + 1, y1, z + 1};
                    emitFace(out, s[0], lo, hi, upper, lower, 3, ao_lower);
                };
                if (me.water != 0) {
                    side(bottom, me.top, my_water, my_water, 3);
                    continue;
                }
                if (me.canopy != 0) side(std::max(bottom, me.ground), me.top, leaves, leaves, 2);
                const int ground_hi = std::min(me.ground, me.top);
                if (bottom < ground_hi) {
                    if (ground_hi - bottom <= 2) side(bottom, ground_hi, ground_top, ground_top, 3);  // escalon: como la cara de arriba
                    else if (top_block == c.deep) side(bottom, ground_hi, deep, deep, 2);            // acantilado
                    else side(bottom, ground_hi, under, deep, 2);                                      // talud: tierra arriba, roca abajo
                }
            }

            // Isla flotante: cesped arriba, panza de roca abajo y los lados que no
            // tapa la isla de la columna de al lado.
            if (c.island_top > 0 && c.island_top >= c.island_bottom) {
                const int itop = c.island_top + 1, ibot = c.island_bottom;
                const BlockDef& idef = blockDef(c.island_block);
                const Color grass = tinted(palette.top[c.island_block], idef.tint == BlockTint::Grass ? c.grass_tint : 0xFFF);
                const Color soil = tinted(palette.side[block::Dirt], 0xFFF);
                const Color rock = tinted(palette.side[c.island_rock], 0xFFF);
                {
                    const int lo[3] = {x, itop - 1, z}, hi[3] = {x + 1, itop, z + 1};
                    emitFace(out, 2, lo, hi, grass, grass, 3, 3);
                }
                {
                    const int lo[3] = {x, ibot, z}, hi[3] = {x + 1, ibot + 1, z + 1};
                    emitFace(out, 3, lo, hi, rock, rock, 1, 1);
                }
                for (const auto& s : kSides) {
                    const Generator::FarColumn& n = columns[static_cast<std::size_t>((x + s[1] + 1) + (z + s[2] + 1) * kBorder)];
                    const auto side = [&](int y0, int y1) {
                        if (y0 >= y1) return;
                        const int lo[3] = {x, y0, z}, hi[3] = {x + 1, y1, z + 1};
                        const bool top_part = y1 >= itop;
                        emitFace(out, s[0], lo, hi, top_part ? soil : rock, rock, 3, 1);
                    };
                    if (n.island_top <= 0 || n.island_top < n.island_bottom) {
                        side(ibot, itop);
                    } else {
                        side(std::max(ibot, n.island_top + 1), itop);    // por encima de la vecina
                        side(ibot, std::min(itop, n.island_bottom));     // por debajo
                    }
                }
            }
        }
    }
}

// --- Gestor -------------------------------------------------------------------------

namespace {

// Lo que comparten las tareas con el gestor (vive mientras haya alguna en vuelo).
struct Shared {
    struct Result {
        std::uint64_t key = 0;
        std::uint64_t epoch = 0;
        std::vector<std::uint32_t> vertices;
        float milliseconds = 0.0f;
        bool from_cache = false;
    };
    std::mutex mutex;
    std::vector<Result> results;
};

}  // namespace

struct FarTerrain::Impl {
    enum class State { Waiting, Building, Built, Uploaded };
    struct Tile {
        int level = 0, tx = 0, tz = 0;
        State state = State::Waiting;
        bool wanted = true;
        bool in_renderer = false;
        float priority = 0.0f;
        std::uint32_t quads = 0;
        std::vector<std::uint32_t> vertices;  // Built: esperando subida
        Clock::time_point stale_since{};
        int x0() const { return tx * kSamples << level; }
        int z0() const { return tz * kSamples << level; }
        int size() const { return kSamples << level; }
    };

    std::shared_ptr<const Generator> generator;
    std::shared_ptr<const FarPalette> palette;
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    std::uint64_t epoch = 1;
    std::unordered_map<std::uint64_t, Tile> tiles;
    std::vector<std::uint64_t> waiting;  // por prioridad (de cerca a lejos)
    int in_flight = 0;
    std::vector<std::uint64_t> removals;
    std::deque<std::uint64_t> uploads;
    bool planned = false;
    Vec3 planned_at{};
    float planned_inner = 0.0f;
    Settings planned_settings{};
    std::filesystem::path cache_folder;
    Stats stats;

    void clear() {
        ++epoch;  // lo que este en vuelo se tira al llegar
        tiles.clear();
        waiting.clear();
        removals.clear();
        uploads.clear();
        in_flight = 0;
        planned = false;
        stats = Stats{};
    }

    // Teselas que hacen falta alrededor de `viewer`.
    void plan(const Vec3& viewer, float inner, const Settings& s) {
        planned = true;
        planned_at = viewer;
        planned_inner = inner;
        planned_settings = s;
        const float distance = s.distance;
        const float k = 4.0f * std::clamp(s.detail, 0.25f, 4.0f);
        struct Want {
            int level, tx, tz;
            float priority;
        };
        std::vector<Want> wanted;
        wanted.reserve(4096);
        const auto visit = [&](auto&& self, int level, int tx, int tz) -> void {
            const float size = static_cast<float>(kSamples << level);
            const float x0 = static_cast<float>(tx) * size, z0 = static_cast<float>(tz) * size;
            const float dx = std::max({x0 - viewer.x, 0.0f, viewer.x - (x0 + size)});
            const float dz = std::max({z0 - viewer.z, 0.0f, viewer.z - (z0 + size)});
            const float dmin = std::sqrt(dx * dx + dz * dz);
            if (dmin > distance) return;
            const float fx = std::max(std::abs(viewer.x - x0), std::abs(viewer.x - (x0 + size)));
            const float fz = std::max(std::abs(viewer.z - z0), std::abs(viewer.z - (z0 + size)));
            const bool inside = std::sqrt(fx * fx + fz * fz) <= inner;  // toda dentro de los chunks de verdad
            const bool split = level > kMinLevel && (inside ? level > kFillLevel : dmin < k * size);
            if (split) {
                for (int c = 0; c < 4; ++c) self(self, level - 1, tx * 2 + (c & 1), tz * 2 + (c >> 1));
                return;
            }
            wanted.push_back({level, tx, tz, dmin});
        };
        const int root = kSamples << kRootLevel;
        const int rx0 = floorDiv(static_cast<int>(std::floor(viewer.x - distance)), root);
        const int rx1 = floorDiv(static_cast<int>(std::floor(viewer.x + distance)), root);
        const int rz0 = floorDiv(static_cast<int>(std::floor(viewer.z - distance)), root);
        const int rz1 = floorDiv(static_cast<int>(std::floor(viewer.z + distance)), root);
        for (int tz = rz0; tz <= rz1; ++tz) {
            for (int tx = rx0; tx <= rx1; ++tx) visit(visit, kRootLevel, tx, tz);
        }

        for (auto& [key, tile] : tiles) tile.wanted = false;
        for (const Want& w : wanted) {
            const std::uint64_t key = tileKey(w.level, w.tx, w.tz);
            auto [it, inserted] = tiles.try_emplace(key);
            Tile& t = it->second;
            if (inserted) {
                t.level = w.level;
                t.tx = w.tx;
                t.tz = w.tz;
            }
            t.wanted = true;
            t.priority = w.priority;
        }
        // Las que ya no hacen falta y no estan en el renderizador se olvidan ya;
        // las dibujadas esperan a que su zona este cubierta (update).
        const auto now = Clock::now();
        for (auto it = tiles.begin(); it != tiles.end();) {
            Tile& t = it->second;
            if (!t.wanted && (t.state == State::Waiting || t.state == State::Built)) {
                it = tiles.erase(it);
                continue;
            }
            if (!t.wanted && t.state == State::Uploaded && t.stale_since == Clock::time_point{}) t.stale_since = now;
            if (t.wanted) t.stale_since = Clock::time_point{};
            ++it;
        }
        waiting.clear();
        for (const auto& [key, t] : tiles) {
            if (t.wanted && t.state == State::Waiting) waiting.push_back(key);
        }
        // De lejos a cerca: se sacan por el final.
        std::sort(waiting.begin(), waiting.end(),
                  [this](std::uint64_t a, std::uint64_t b) { return tiles.at(a).priority > tiles.at(b).priority; });
    }

    void submit(std::uint64_t key, Tile& tile) {
        tile.state = State::Building;
        ++in_flight;
        const std::shared_ptr<Shared> out = shared;
        const std::shared_ptr<const Generator> gen = generator;
        const std::shared_ptr<const FarPalette> pal = palette;
        const std::filesystem::path folder = planned_settings.disk_cache ? cache_folder : std::filesystem::path{};
        const bool sea = planned_settings.sea;
        const int level = tile.level, tx = tile.tx, tz = tile.tz;
        const std::uint64_t job_epoch = epoch;
        jobs::schedule(
            [out, gen, pal, folder, sea, level, tx, tz, key, job_epoch]() {
                const auto start = Clock::now();
                Shared::Result r;
                r.key = key;
                r.epoch = job_epoch;
                try {
                    std::vector<Generator::FarColumn> columns;
                    const std::filesystem::path file = folder.empty() ? std::filesystem::path{} : cacheFile(folder, level, tx, tz);
                    r.from_cache = !file.empty() && readCache(file, level, tx, tz, columns);
                    if (!r.from_cache) {
                        sampleTile(*gen, level, tx, tz, columns);
                        if (!file.empty()) writeCache(file, level, tx, tz, columns);
                    }
                    meshTile(columns, level, *pal, sea, r.vertices);
                } catch (...) {
                    r.vertices.clear();
                }
                r.milliseconds = static_cast<float>(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                std::lock_guard lock(out->mutex);
                out->results.push_back(std::move(r));
            },
            nullptr, jobs::Priority::Background, "voxel.far.tile");
    }

    void collect() {
        std::vector<Shared::Result> done;
        {
            std::lock_guard lock(shared->mutex);
            done.swap(shared->results);
        }
        for (Shared::Result& r : done) {
            if (r.epoch != epoch) continue;
            in_flight = std::max(0, in_flight - 1);
            stats.last_tile_ms = r.milliseconds;
            ++(r.from_cache ? stats.from_cache : stats.generated);
            const auto it = tiles.find(r.key);
            if (it == tiles.end() || it->second.state != State::Building) continue;
            Tile& t = it->second;
            if (!t.wanted) {
                tiles.erase(it);
                continue;
            }
            t.vertices = std::move(r.vertices);
            t.state = State::Built;
            uploads.push_back(r.key);
        }
    }

    // Quita las viejas cuya zona ya esta cubierta por las nuevas (o que llevan
    // mucho esperando).
    void retireStale() {
        std::vector<std::uint64_t> stale;
        std::vector<const Tile*> not_ready;
        for (const auto& [key, t] : tiles) {
            if (!t.wanted && t.state == State::Uploaded) stale.push_back(key);
            else if (t.wanted && t.state != State::Uploaded) not_ready.push_back(&t);
        }
        if (stale.empty()) return;
        const auto now = Clock::now();
        for (const std::uint64_t key : stale) {
            const Tile& t = tiles.at(key);
            bool covered = true;
            const bool too_old = now - t.stale_since > std::chrono::seconds(4);
            if (!too_old) {
                if (not_ready.size() > 512) continue;  // mucho por hacer: luego
                const int ax0 = t.x0(), az0 = t.z0(), ax1 = ax0 + t.size(), az1 = az0 + t.size();
                for (const Tile* n : not_ready) {
                    const int bx0 = n->x0(), bz0 = n->z0(), bx1 = bx0 + n->size(), bz1 = bz0 + n->size();
                    if (ax0 < bx1 && bx0 < ax1 && az0 < bz1 && bz0 < az1) {
                        covered = false;
                        break;
                    }
                }
            }
            if (!covered) continue;
            if (t.in_renderer) removals.push_back(key);
            tiles.erase(key);
        }
    }
};

FarTerrain::FarTerrain() : impl_(std::make_unique<Impl>()) {}
FarTerrain::~FarTerrain() = default;  // las tareas en vuelo solo tocan su Shared

void FarTerrain::reset(std::shared_ptr<const Generator> generator) {
    Impl& d = *impl_;
    d.clear();
    d.generator = std::move(generator);
    d.cache_folder = d.generator ? cacheFolder(*d.generator) : std::filesystem::path{};
}

const Generator* FarTerrain::generator() const { return impl_->generator.get(); }

void FarTerrain::setPalette(std::shared_ptr<const FarPalette> palette) {
    Impl& d = *impl_;
    const bool rebuild = d.palette != nullptr;
    d.palette = std::move(palette);
    if (rebuild) {
        // Otras texturas: otras mallas. Las dibujadas se quitan.
        for (const auto& [key, t] : d.tiles) {
            if (t.in_renderer) d.removals.push_back(key);
        }
        std::vector<std::uint64_t> keep = std::move(d.removals);
        d.clear();
        d.removals = std::move(keep);
    }
}

bool FarTerrain::hasPalette() const { return impl_->palette != nullptr; }

FarTerrain::Settings FarTerrain::currentSettings() {
    Settings s;
    s.distance = static_cast<float>(g_far_distance.get());
    s.detail = g_far_detail.get();
    s.disk_cache = g_far_cache.get();
    s.sea = g_far_sea.get();
    return s;
}

void FarTerrain::update(const Vec3& viewer, float inner_radius, const Settings& settings) {
    Impl& d = *impl_;
    const bool supported = d.generator && (d.generator->profile() == VoxelGenerator::Classic || d.generator->profile() == VoxelGenerator::Adventure);
    if (!supported || !d.palette || settings.distance <= 0.0f) {
        // Apagado: fuera lo dibujado.
        if (!d.tiles.empty()) {
            for (const auto& [key, t] : d.tiles) {
                if (t.in_renderer) d.removals.push_back(key);
            }
            std::vector<std::uint64_t> keep = std::move(d.removals);
            d.clear();
            d.removals = std::move(keep);
        }
        return;
    }
    // Cambio de ajustes que cambia las mallas: rehacer todo (lo viejo sigue
    // hasta que lo nuevo cubre su zona).
    if (d.planned && (settings.sea != d.planned_settings.sea || settings.detail != d.planned_settings.detail ||
                      settings.disk_cache != d.planned_settings.disk_cache)) {
        if (settings.sea != d.planned_settings.sea) {
            for (auto& [key, t] : d.tiles) {
                if (t.in_renderer) d.removals.push_back(key);
            }
            std::vector<std::uint64_t> keep = std::move(d.removals);
            d.clear();
            d.removals = std::move(keep);
        } else {
            d.planned = false;
        }
    }
    d.collect();
    const float moved_x = viewer.x - d.planned_at.x, moved_z = viewer.z - d.planned_at.z;
    if (!d.planned || moved_x * moved_x + moved_z * moved_z > 16.0f * 16.0f || std::abs(inner_radius - d.planned_inner) > 15.0f ||
        settings.distance != d.planned_settings.distance || settings.detail != d.planned_settings.detail) {
        d.plan(viewer, inner_radius, settings);
    }
    // Lanzar las mas cercanas (un limite en vuelo: se pueden reordenar al moverse).
    const int max_in_flight = std::max(2, jobs::workerCount());
    while (d.in_flight < max_in_flight && !d.waiting.empty()) {
        const std::uint64_t key = d.waiting.back();
        d.waiting.pop_back();
        const auto it = d.tiles.find(key);
        if (it == d.tiles.end() || !it->second.wanted || it->second.state != Impl::State::Waiting) continue;
        d.submit(key, it->second);
    }
    d.retireStale();
}

void FarTerrain::takeUpdates(std::vector<Update>& out, std::size_t byte_budget) {
    Impl& d = *impl_;
    for (const std::uint64_t key : d.removals) out.push_back(Update{key, Vec3{}, 1.0f, {}});
    d.removals.clear();
    std::size_t bytes = 0;
    while (!d.uploads.empty()) {
        const std::uint64_t key = d.uploads.front();
        const auto it = d.tiles.find(key);
        if (it == d.tiles.end() || it->second.state != Impl::State::Built) {
            d.uploads.pop_front();
            continue;
        }
        Impl::Tile& t = it->second;
        const std::size_t size = t.vertices.size() * sizeof(std::uint32_t);
        if (bytes > 0 && bytes + size > byte_budget) break;
        d.uploads.pop_front();
        bytes += size;
        t.state = Impl::State::Uploaded;
        t.quads = static_cast<std::uint32_t>(t.vertices.size() / 8);
        if (t.vertices.empty()) continue;
        t.in_renderer = true;
        const Vec3 origin{static_cast<float>(t.x0()), 0.0f, static_cast<float>(t.z0())};
        out.push_back(Update{key, origin, static_cast<float>(1 << t.level), std::move(t.vertices)});
        t.vertices = {};
    }
}

FarTerrain::Stats FarTerrain::stats() const {
    const Impl& d = *impl_;
    Stats s = d.stats;
    for (const auto& [key, t] : d.tiles) {
        if (!t.wanted) continue;
        ++s.tiles;
        if (t.state == Impl::State::Uploaded) {
            ++s.ready;
            s.quads += t.quads;
        } else {
            ++s.pending;
        }
    }
    return s;
}

}  // namespace cramion::voxel
