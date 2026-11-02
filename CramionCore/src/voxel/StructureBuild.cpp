// Construccion de las piezas de las estructuras (Structures.h): cada funcion
// llena un plano local (fachada hacia -z) con bloques y deja los puntos para
// el juego (aldeanos, mercaderes, cofres, jefes...). Estilo Cube World: casas
// de entramado con tejados escalonados de tejas de colores, interiores
// amueblados (camas, mesas, sillas, estanterias, chimeneas encendidas,
// alfombras, cocina), murallas con almenas, torres con tejado en piramide,
// castillo con salon del trono y mazmorras de piedra tallada.
//
// Todo es determinista: el azar sale de ctx.seed.

#include "Structures.h"

#include <algorithm>
#include <cmath>

namespace cramion::voxel {

namespace B = block;

namespace {

std::uint32_t mix32(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

}  // namespace

float BuildContext::rand(int a, int b, int c) const {
    const std::uint32_t h = mix32(seed ^ mix32(static_cast<std::uint32_t>(a) * 0x9E3779B1u ^
                                               mix32(static_cast<std::uint32_t>(b) * 0x85EBCA77u ^
                                                     static_cast<std::uint32_t>(c) * 0xC2B2AE3Du)));
    return static_cast<float>(h & 0xFFFFFF) / 16777216.0f;
}

int BuildContext::randInt(int lo, int hi, int a, int b) const {
    return lo + std::min(hi - lo, static_cast<int>(rand(a, b, 77) * static_cast<float>(hi - lo + 1)));
}

namespace build {

namespace {

// Celdas reservadas (escaleras, pasos): los muebles no las ocupan.
struct Reserve {
    const Blueprint* bp = nullptr;
    std::vector<bool> mask;
    explicit Reserve(const Blueprint& b) : bp(&b), mask(b.cells.size(), false) {}
    void mark(int x, int y, int z) {
        if (bp->inside(x, y, z)) mask[bp->index(x, y, z)] = true;
    }
    void markColumn(int x, int y0, int y1, int z) {
        for (int y = y0; y <= y1; ++y) mark(x, y, z);
    }
    bool has(int x, int y, int z) const { return bp->inside(x, y, z) && mask[bp->index(x, y, z)]; }
};

// Pone un mueble si la celda esta libre (aire y sin reservar).
bool place(Blueprint& bp, const Reserve& r, int x, int y, int z, BlockId id) {
    if (!bp.inside(x, y, z) || bp.get(x, y, z) != B::Air || r.has(x, y, z)) return false;
    bp.set(x, y, z, id);
    return true;
}

// Escalera recta que sube hacia +z desde el suelo y0: n escalones; el ultimo
// queda al nivel del piso de arriba (y0 + n). Abre el hueco y reserva el paso.
void stairsZ(Blueprint& bp, Reserve& r, int x, int y0, int z0, int n, BlockId step) {
    for (int j = 0; j < n; ++j) {
        bp.set(x, y0 + 1 + j, z0 + j, step);
        for (int h = 1; h <= 3; ++h) {
            bp.set(x, y0 + 1 + j + h, z0 + j, B::Air);
            r.mark(x, y0 + 1 + j + h, z0 + j);
        }
        r.mark(x, y0 + 1 + j, z0 + j);
    }
    // Llegada arriba y la celda de antes de subir.
    r.markColumn(x, y0 + n + 1, y0 + n + 3, z0 + n);
    r.markColumn(x, y0 + 1, y0 + 3, z0 - 1);
}

// --- Paletas de casas ----------------------------------------------------------------

struct Palette {
    BlockId found = B::Cobblestone;
    BlockId wall_low = B::Plaster;
    BlockId wall = B::Plaster;
    BlockId beam = B::Timber;
    BlockId floor = B::OakPlanks;
    BlockId roof = B::RoofRed;
    BlockId ridge = B::Timber;
    BlockId window = B::Glass;
    BlockId chimney = B::Bricks;
    BlockId rug = B::RugRed;
    bool posts = true;
    bool flat = false;   // tejado plano con parapeto (desierto)
    bool snowy = false;  // tejado nevado
};

Palette paletteFor(const BuildContext& ctx, int variant) {
    Palette p;
    p.roof = ctx.roof;
    p.rug = variant % 2 == 0 ? B::RugRed : B::RugBlue;
    switch (ctx.style) {
        case BuildStyle::Snow:
            p.wall_low = p.wall = B::SprucePlanks;
            p.beam = p.ridge = B::SpruceLog;
            p.floor = B::SprucePlanks;
            p.chimney = B::Cobblestone;
            p.snowy = true;
            if (variant % 3 == 0) p.wall_low = B::Cobblestone;
            break;
        case BuildStyle::Desert:
            p.found = B::Sandstone;
            p.wall_low = p.wall = variant % 2 == 0 ? B::MudBricks : B::Plaster;
            p.beam = B::Terracotta;
            p.floor = B::Sandstone;
            p.window = B::Air;
            p.posts = false;
            p.flat = true;
            p.chimney = B::MudBricks;
            break;
        case BuildStyle::Beach:
            p.found = B::Cobblestone;
            p.wall_low = p.wall = B::OakPlanks;
            p.beam = p.ridge = B::OakLog;
            p.roof = B::Thatch;
            p.chimney = B::Cobblestone;
            break;
        case BuildStyle::City:
            p.found = B::StoneBricks;
            p.wall_low = variant % 3 == 2 ? B::Bricks : B::StoneBricks;
            p.wall = variant % 4 == 3 ? B::Bricks : B::Plaster;
            p.beam = p.ridge = B::Timber;
            p.floor = variant % 2 == 0 ? B::DarkPlanks : B::OakPlanks;
            break;
        default:
            if (variant % 3 == 1) p.wall_low = B::Cobblestone;
            if (variant % 5 == 4) p.wall_low = p.wall = B::OakPlanks;
            break;
    }
    return p;
}

// --- Casa: estructura (muros, pisos, tejado, chimenea, escaleras) ---------------------

struct Shell {
    int x0 = 1, x1 = 1, z0 = 1, z1 = 1;   // cuerpo (muros incluidos)
    int ix0 = 2, ix1 = 2, iz0 = 2, iz1 = 2;  // interior
    int floors = 1;
    int top = 4;
    int door_x = 2;
    bool chimney = false;
    int chimney_x = 0, chimney_z = 0;
    bool stairs = false;
    int floorY(int f) const { return 4 * f; }
};

Shell houseShell(BuildContext& ctx, Reserve*& reserve_out, std::unique_ptr<Reserve>& holder, int w, int d, int floors,
                 const Palette& p, bool chimney) {
    Shell s;
    const int W = w + 2, D = d + 2;
    const int roof_layers = p.flat ? 2 : (D + 1) / 2;
    const int H = 4 * floors + roof_layers + 4;
    ctx.bp = Blueprint(W, H, D);
    holder = std::make_unique<Reserve>(ctx.bp);
    reserve_out = holder.get();
    Blueprint& bp = ctx.bp;
    Reserve& r = *holder;
    s.x0 = 1;
    s.x1 = W - 2;
    s.z0 = 1;
    s.z1 = D - 2;
    s.ix0 = s.x0 + 1;
    s.ix1 = s.x1 - 1;
    s.iz0 = s.z0 + 1;
    s.iz1 = s.z1 - 1;
    s.floors = floors;
    s.top = 4 * floors;
    s.door_x = s.x0 + w / 2;
    if ((s.door_x - s.x0) % 4 == 0) ++s.door_x;

    // Cimiento y suelo.
    for (int z = s.z0; z <= s.z1; ++z) {
        for (int x = s.x0; x <= s.x1; ++x) {
            const bool ring = x == s.x0 || x == s.x1 || z == s.z0 || z == s.z1;
            bp.set(x, 0, z, ring ? p.found : p.floor);
        }
    }
    // Muros por piso: postes, vigas, relleno y ventanas.
    for (int f = 0; f < floors; ++f) {
        const int base = 4 * f;
        for (int y = base + 1; y <= base + 4; ++y) {
            const bool band = y == base + 4;
            for (int z = s.z0; z <= s.z1; ++z) {
                for (int x = s.x0; x <= s.x1; ++x) {
                    const bool on_x = x == s.x0 || x == s.x1;
                    const bool on_z = z == s.z0 || z == s.z1;
                    if (!on_x && !on_z) {
                        bp.set(x, y, z, band ? p.floor : B::Air);
                        continue;
                    }
                    const bool corner = on_x && on_z;
                    const int along = on_z ? x - s.x0 : z - s.z0;
                    const int len = on_z ? w : d;
                    const bool post = p.posts && (along % 4 == 0 || along == len - 1);
                    BlockId m = f == 0 ? p.wall_low : p.wall;
                    if (corner || band || post) m = p.beam;
                    if (!corner && !band && !post && along % 4 == 2 && along < len - 1 && (y == base + 2 || y == base + 3)) {
                        m = p.window;
                    }
                    if (!p.posts && corner) m = p.wall_low;
                    if (!p.posts && band) m = p.beam;
                    bp.set(x, y, z, m);
                }
            }
        }
    }
    // Puerta (2 de alto) con dintel, escalon y farol.
    bp.set(s.door_x, 1, s.z0, B::Door);
    bp.set(s.door_x, 2, s.z0, B::Door);
    bp.set(s.door_x, 3, s.z0, p.beam);
    bp.set(s.door_x, 0, s.z0 - 1, p.found);
    bp.set(s.door_x + 1, 3, s.z0 - 1, B::Lantern);
    r.markColumn(s.door_x, 1, 3, s.iz0);
    r.markColumn(s.door_x - 1, 1, 2, s.iz0);
    r.markColumn(s.door_x + 1, 1, 2, s.iz0);

    // Tejado.
    const int top = s.top;
    if (p.flat) {
        for (int z = s.z0; z <= s.z1; ++z) {
            for (int x = s.x0; x <= s.x1; ++x) {
                const bool ring = x == s.x0 || x == s.x1 || z == s.z0 || z == s.z1;
                bp.set(x, top, z, ring ? p.beam : p.found);
                if (ring && ((x + z) % 2 == 0)) bp.set(x, top + 1, z, p.beam);
            }
        }
        // Toldo sobre la puerta.
        const BlockId cloth = ctx.rand(5) < 0.5f ? B::ClothBlue : (ctx.rand(6) < 0.5f ? B::ClothRed : B::ClothYellow);
        for (int x = s.door_x - 1; x <= s.door_x + 1; ++x) bp.set(x, 3, s.z0 - 1, cloth);
        bp.set(s.door_x + 1, 3, s.z0 - 1, cloth);
        bp.set(s.door_x + 2, 2, s.z0 - 1, B::Lantern);
    } else {
        for (int k = 0;; ++k) {
            const int za = k, zb = D - 1 - k;
            if (za > zb) break;
            const int y = top + k;
            const bool ridge = za >= zb - 1;
            for (int x = 0; x < W; ++x) {
                BlockId t = p.roof;
                if (p.snowy && k > 0) t = B::Snow;
                if (ridge) t = p.snowy ? B::Snow : p.ridge;
                if ((x == 0 || x == W - 1) && !ridge && !p.snowy && p.roof != B::Thatch) t = p.beam;
                bp.set(x, y, za, t);
                bp.set(x, y, zb, t);
            }
            if (k == 0) continue;
            for (int z = za + 1; z < zb; ++z) {
                for (int x = s.x0; x <= s.x1; ++x) {
                    if (x == s.x0 || x == s.x1) {
                        BlockId g = p.wall;
                        if (z == D / 2 && k == 2 && zb - za > 3) g = p.window == B::Air ? B::Air : B::Glass;
                        else if (z == D / 2 && p.posts) g = p.beam;
                        bp.set(x, y, z, g);
                    } else {
                        bp.set(x, y, z, B::Air);
                    }
                }
            }
        }
    }
    // Chimenea en la esquina de atras a la derecha (fuego en la planta baja).
    if (chimney) {
        s.chimney = true;
        s.chimney_x = s.ix1;
        s.chimney_z = s.iz1;
        const int ridge_y = p.flat ? top + 2 : top + (D - 1) / 2 + 2;
        for (int y = 0; y <= ridge_y; ++y) bp.set(s.chimney_x, y, s.chimney_z, p.chimney);
        bp.set(s.chimney_x, 1, s.chimney_z, B::Fire);
        bp.set(s.chimney_x, 0, s.chimney_z, B::Bricks);
        for (int f = 0; f < floors; ++f) r.markColumn(s.chimney_x - 1, 4 * f + 1, 4 * f + 1, s.chimney_z);
    }
    // Escaleras entre pisos (junto al muro izquierdo, subiendo hacia el fondo).
    if (floors > 1 && d >= 6) {
        s.stairs = true;
        for (int f = 0; f + 1 < floors; ++f) {
            stairsZ(bp, r, s.ix0, 4 * f, s.iz0, 4, p.floor);
            // Barandilla del hueco arriba.
            for (int z = s.iz0; z < s.iz0 + 3; ++z) bp.set(s.ix0 + 1, 4 * f + 5, z, B::Fence);
        }
    }
    return s;
}

// Salon con cocina (planta baja) o dormitorio. Devuelve donde hay sitio para
// los puntos.
struct RoomSpots {
    float npc_x = 0, npc_z = 0;
    bool bed = false;
    int bed_x = 0, bed_z = 0;
};

RoomSpots furnishLiving(BuildContext& ctx, const Shell& s, Reserve& r, int f, const Palette& p, int variant) {
    Blueprint& bp = ctx.bp;
    const int y = 4 * f + 1;
    RoomSpots spots;
    const int tx = (s.ix0 + s.ix1 + 1) / 2;
    const int tz = std::min(s.iz1 - 1, (s.iz0 + s.iz1) / 2 + 1);
    // Alfombra bajo la mesa.
    for (int z = tz - 1; z <= tz + 1; ++z)
        for (int x = tx - 1; x <= tx + 1; ++x)
            if (bp.get(x, y - 1, z) == p.floor && x >= s.ix0 && x <= s.ix1 && z >= s.iz0 && z <= s.iz1) bp.set(x, y - 1, z, p.rug);
    // Mesa con velas y sillas.
    if (place(bp, r, tx, y, tz, B::Table)) bp.set(tx, y + 1, tz, B::Candles);
    if (s.ix1 - s.ix0 >= 5) place(bp, r, tx + 1, y, tz, B::Table);
    place(bp, r, tx - 1, y, tz, B::Chair);
    place(bp, r, tx + (s.ix1 - s.ix0 >= 5 ? 2 : 1), y, tz, B::Chair);
    place(bp, r, tx, y, tz + 1, B::Chair);
    // Cocina en el muro de atras: horno, encimera, barril.
    place(bp, r, s.ix0, y, s.iz1, B::Oven);
    place(bp, r, s.ix0 + 1, y, s.iz1, B::Counter);
    if (!place(bp, r, s.ix0 + 2, y, s.iz1, B::Barrel)) place(bp, r, s.ix0 + 1, y + 1, s.iz1, B::Barrel);
    place(bp, r, s.ix0 + 1, y + 1, s.iz1, variant % 2 ? B::Shelf : B::Crate);
    // Rincon de delante a la derecha: caja y estanteria.
    place(bp, r, s.ix1, y, s.iz0, variant % 2 ? B::Crate : B::Barrel);
    if (place(bp, r, s.ix1, y, s.iz0 + 2, B::Bookshelf)) place(bp, r, s.ix1, y + 1, s.iz0 + 2, B::Bookshelf);
    // Farol en el techo.
    place(bp, r, tx, y + 2, tz, B::Lantern);
    spots.npc_x = static_cast<float>(tx);
    spots.npc_z = static_cast<float>(tz - 1);
    return spots;
}

RoomSpots furnishBedroom(BuildContext& ctx, const Shell& s, Reserve& r, int f, const Palette& p, int variant, bool shared_room) {
    Blueprint& bp = ctx.bp;
    const int y = 4 * f + 1;
    RoomSpots spots;
    // Cama contra el muro derecho, con el cabecero al fondo (la chimenea ocupa la esquina).
    const int bz1 = s.chimney ? s.iz1 - 1 : s.iz1;
    if (bz1 - 1 >= s.iz0 && place(bp, r, s.ix1, y, bz1, B::Bed)) {
        place(bp, r, s.ix1, y, bz1 - 1, B::Bed);
        spots.bed = true;
        spots.bed_x = s.ix1;
        spots.bed_z = bz1;
        place(bp, r, s.ix1, y, bz1 - 2, B::Chest);
        // Mesilla con velas.
        if (s.chimney ? false : place(bp, r, s.ix1 - 1, y, bz1, B::Table)) bp.set(s.ix1 - 1, y + 1, bz1, B::Candles);
    }
    if (!shared_room) {
        // Alfombra en el centro.
        const int cx = (s.ix0 + s.ix1) / 2 + 1, cz = (s.iz0 + s.iz1) / 2;
        for (int z = cz - 1; z <= cz + 1; ++z)
            for (int x = cx - 1; x <= cx; ++x)
                if (bp.get(x, y - 1, z) == p.floor) bp.set(x, y - 1, z, variant % 2 ? B::RugBlue : B::RugRed);
        // Armario (estanteria de 2) y baul en el fondo izquierdo.
        if (place(bp, r, s.ix0 + 1, y, s.iz1, B::Bookshelf)) place(bp, r, s.ix0 + 1, y + 1, s.iz1, B::Bookshelf);
        place(bp, r, s.ix0 + 2, y, s.iz1, B::Barrel);
        // Escritorio junto a la ventana de delante.
        if (place(bp, r, s.ix1, y, s.iz0, B::Table)) bp.set(s.ix1, y + 1, s.iz0, B::Candles);
        place(bp, r, s.ix1 - 1, y, s.iz0, B::Chair);
        place(bp, r, cx, y + 2, cz, B::Lantern);
        spots.npc_x = static_cast<float>(cx);
        spots.npc_z = static_cast<float>(cz);
    }
    return spots;
}

int dirFace(int d) { return d; }

}  // namespace

// --- Casa ------------------------------------------------------------------------------

void house(BuildContext& ctx, int w, int d, int floors, const std::string& npc_tag) {
    const int variant = static_cast<int>(ctx.rand(1) * 1000.0f);
    const Palette p = paletteFor(ctx, variant);
    Reserve* r = nullptr;
    std::unique_ptr<Reserve> holder;
    const bool chimney = ctx.style != BuildStyle::Desert && ctx.style != BuildStyle::Beach ? true : ctx.rand(2) < 0.4f;
    const Shell s = houseShell(ctx, r, holder, w, d, floors, p, chimney);
    const RoomSpots living = furnishLiving(ctx, s, *r, 0, p, variant);
    ctx.point("npc", living.npc_x, 1, living.npc_z, 2, npc_tag);
    if (floors == 1) {
        const RoomSpots bed = furnishBedroom(ctx, s, *r, 0, p, variant, true);
        if (bed.bed) ctx.point("cama", static_cast<float>(bed.bed_x), 2, static_cast<float>(bed.bed_z), 0);
    } else {
        for (int f = 1; f < floors; ++f) {
            const RoomSpots bed = furnishBedroom(ctx, s, *r, f, p, variant + f, false);
            if (bed.bed) ctx.point("cama", static_cast<float>(bed.bed_x), static_cast<float>(4 * f + 2), static_cast<float>(bed.bed_z), 0);
            ctx.point("npc", bed.npc_x, static_cast<float>(4 * f + 1), bed.npc_z, 0, f == 1 ? "aldeano" : "nino");
        }
    }
    // Macetas de flores bajo las ventanas de la fachada (planta baja).
    if (ctx.style == BuildStyle::Temperate || ctx.style == BuildStyle::City) {
        for (int x = s.x0 + 2; x < s.x1; x += 4) {
            if (x == s.door_x || ctx.rand(x, 9) < 0.35f) continue;
            ctx.bp.set(x, 1, s.z0 - 1, B::Bush);
        }
    }
    (void)dirFace;
}

// --- Taberna ----------------------------------------------------------------------------

void tavern(BuildContext& ctx) {
    const int variant = static_cast<int>(ctx.rand(1) * 1000.0f);
    Palette p = paletteFor(ctx, variant);
    Reserve* rp = nullptr;
    std::unique_ptr<Reserve> holder;
    const int w = 13, d = 11;
    const Shell s = houseShell(ctx, rp, holder, w, d, 2, p, true);
    Reserve& r = *rp;
    Blueprint& bp = ctx.bp;
    const int y = 1;
    // Barra: mostrador a lo ancho, con el tabernero detras y barriles al fondo.
    const int bar_z = s.iz1 - 2;
    for (int x = s.ix0 + 3; x <= s.ix1 - 1; ++x) place(bp, r, x, y, bar_z, B::Counter);
    for (int x = s.ix0 + 3; x <= s.ix1 - 1; x += 3) bp.set(x, y + 1, bar_z, B::Candles);
    for (int x = s.ix0 + 2; x <= s.ix1 - 1; ++x) {
        if (place(bp, r, x, y, s.iz1, (x % 3 == 0) ? B::Shelf : B::Barrel)) place(bp, r, x, y + 1, s.iz1, x % 2 ? B::Barrel : B::Shelf);
    }
    const int keeper_x = (s.ix0 + 3 + s.ix1 - 1) / 2;
    ctx.point("npc", static_cast<float>(keeper_x), 1, static_cast<float>(bar_z + 1), 0, "tabernero");
    // Mesas con bancos y velas (zona de delante).
    const int tables[3][2] = {{s.ix0 + 3, s.iz0 + 2}, {s.ix1 - 2, s.iz0 + 2}, {s.ix0 + 6, s.iz0 + 4}};
    int guests = 0;
    for (const auto& t : tables) {
        const int tx = t[0], tz = t[1];
        if (!place(bp, r, tx, y, tz, B::Table)) continue;
        bp.set(tx, y + 1, tz, B::Candles);
        place(bp, r, tx + 1, y, tz, B::Table);
        place(bp, r, tx - 1, y, tz, B::Chair);
        place(bp, r, tx + 2, y, tz, B::Chair);
        place(bp, r, tx, y, tz - 1, B::Bench);
        place(bp, r, tx + 1, y, tz - 1, B::Bench);
        place(bp, r, tx, y, tz + 1, B::Bench);
        place(bp, r, tx + 1, y, tz + 1, B::Bench);
        place(bp, r, tx, y + 2, tz, B::Lantern);
        if (guests++ < 3) ctx.point("npc", static_cast<float>(tx - 1), 1, static_cast<float>(tz), 1, guests == 2 ? "bardo" : "aldeano");
        for (int z = tz - 1; z <= tz + 1; ++z)
            for (int x = tx - 1; x <= tx + 2; ++x)
                if (bp.get(x, 0, z) == p.floor) bp.set(x, 0, z, B::RugRed);
    }
    // Estandartes en la fachada.
    bp.set(s.door_x - 2, 3, s.z0, B::BannerRed);
    bp.set(s.door_x + 2, 3, s.z0, B::BannerRed);
    bp.set(s.door_x - 2, 2, s.z0, B::BannerRed);
    bp.set(s.door_x + 2, 2, s.z0, B::BannerRed);
    // Arriba: habitaciones con camas.
    const int y2 = 5;
    int beds = 0;
    for (int x = s.ix0 + 2; x + 1 <= s.ix1 - 1 && beds < 4; x += 3) {
        if (place(bp, r, x, y2, s.iz1, B::Bed)) {
            place(bp, r, x, y2, s.iz1 - 1, B::Bed);
            place(bp, r, x + 1, y2, s.iz1, B::Chest);
            ctx.point("cama", static_cast<float>(x), 6, static_cast<float>(s.iz1), 0);
            ++beds;
        }
    }
    for (int x = s.ix0 + 3; x <= s.ix1 - 2; x += 4) place(bp, r, x, y2 + 2, (s.iz0 + s.iz1) / 2, B::Lantern);
    for (int x = s.ix0 + 2; x <= s.ix1 - 1; ++x)
        if (bp.get(x, 4, (s.iz0 + s.iz1) / 2) == p.floor) bp.set(x, 4, (s.iz0 + s.iz1) / 2, B::RugBlue);
    place(bp, r, s.ix1, y2, s.iz0, B::Table);
    place(bp, r, s.ix1 - 1, y2, s.iz0, B::Chair);
    ctx.point("npc", static_cast<float>(s.ix1 - 2), 5, static_cast<float>(s.iz0 + 1), 0, "aldeano");
}

// --- Herreria ---------------------------------------------------------------------------

void smithy(BuildContext& ctx, bool city) {
    const int variant = static_cast<int>(ctx.rand(1) * 1000.0f);
    Palette p = paletteFor(ctx, variant);
    if (ctx.style != BuildStyle::Desert) p.wall_low = B::Cobblestone;
    Reserve* rp = nullptr;
    std::unique_ptr<Reserve> holder;
    const int w = 10, d = 8;
    const Shell s = houseShell(ctx, rp, holder, w, d, 1, p, true);
    Reserve& r = *rp;
    Blueprint& bp = ctx.bp;
    // Fragua bajo la chimenea (en vez del fuego) con campana de ladrillo.
    bp.set(s.chimney_x, 1, s.chimney_z, B::Forge);
    bp.set(s.chimney_x - 1, 1, s.chimney_z, B::Forge);
    bp.set(s.chimney_x - 1, 2, s.chimney_z, B::Bricks);
    bp.set(s.chimney_x - 1, 3, s.chimney_z, B::Bricks);
    bp.set(s.chimney_x - 2, 3, s.chimney_z, B::Bricks);
    // Yunque, cubo de agua (barril), armeros y mostrador.
    const int ax = s.ix1 - 1, az = s.iz1 - 2;
    bp.set(ax, 1, az, B::Anvil);
    place(bp, r, s.ix1, 1, az, B::Barrel);
    for (int z = s.iz0 + 1; z <= s.iz1; ++z) {
        if (place(bp, r, s.ix0, 1, z, z % 2 ? B::WeaponRack : B::Crate) && z % 2) place(bp, r, s.ix0, 2, z, B::WeaponRack);
    }
    for (int x = s.door_x + 1; x <= std::min(s.door_x + 3, s.ix1); ++x) place(bp, r, x, 1, s.iz0, B::Counter);
    place(bp, r, s.door_x - 2, 1, s.iz1, B::Chest);
    place(bp, r, (s.ix0 + s.ix1) / 2, 3, (s.iz0 + s.iz1) / 2, B::Lantern);
    // Ventanas grandes en la fachada (taller abierto).
    for (int x = s.door_x + 1; x < s.x1; ++x) {
        if (bp.get(x, 2, s.z0) != p.beam) {
            bp.set(x, 2, s.z0, B::Air);
            bp.set(x, 3, s.z0, B::Air);
        }
    }
    ctx.point("npc", static_cast<float>(ax - 1), 1, static_cast<float>(az), 1, "herrero");
    ctx.point("mercader", static_cast<float>(s.door_x + 2), 1, static_cast<float>(s.iz0 + 1), 0, "armas");
    if (city) ctx.point("npc", static_cast<float>(s.ix0 + 1), 1, static_cast<float>(s.iz0 + 2), 3, "aprendiz");
}

// --- Tienda -------------------------------------------------------------------------------

void shop(BuildContext& ctx, const std::string& kind) {
    const int variant = static_cast<int>(ctx.rand(1) * 1000.0f);
    const Palette p = paletteFor(ctx, variant);
    Reserve* rp = nullptr;
    std::unique_ptr<Reserve> holder;
    const int w = 9, d = 8;
    const Shell s = houseShell(ctx, rp, holder, w, d, 2, p, false);
    Reserve& r = *rp;
    Blueprint& bp = ctx.bp;
    // Mostrador a lo ancho y expositores al fondo.
    const int cz = s.iz0 + 2;
    for (int x = s.ix0 + 2; x <= s.ix1; ++x) place(bp, r, x, 1, cz, B::Counter);
    bp.set(s.ix1, 2, cz, B::Candles);
    const BlockId display = kind == "pociones" ? B::Shelf : (kind == "armaduras" ? B::WeaponRack : B::Crate);
    for (int x = s.ix0 + 1; x <= s.ix1; ++x) {
        if (place(bp, r, x, 1, s.iz1, x % 2 ? display : B::Barrel)) place(bp, r, x, 2, s.iz1, display);
    }
    place(bp, r, s.ix1, 1, s.iz1 - 1, B::Chest);
    place(bp, r, (s.ix0 + s.ix1) / 2 + 1, 3, s.iz0 + 1, B::Lantern);
    ctx.point("mercader", static_cast<float>((s.ix0 + s.ix1) / 2 + 1), 1, static_cast<float>(cz + 1), 0, kind);
    // Toldo y estandarte del color de la tienda.
    const BlockId cloth = kind == "pociones" ? B::ClothBlue : (kind == "armaduras" ? B::ClothRed : B::ClothYellow);
    for (int x = s.door_x - 2; x <= s.door_x + 2; ++x) {
        if (x == s.door_x + 1) continue;  // el farol
        bp.set(x, 3, s.z0 - 1, (x % 2) ? cloth : B::ClothWhite);
    }
    // Arriba: dormitorio.
    const RoomSpots bed = furnishBedroom(ctx, s, r, 1, p, variant, false);
    if (bed.bed) ctx.point("cama", static_cast<float>(bed.bed_x), 6, static_cast<float>(bed.bed_z), 0);
}

// --- Establo (monturas) ------------------------------------------------------------------

void stable(BuildContext& ctx) {
    Palette p = paletteFor(ctx, 1);
    p.wall_low = p.wall = ctx.style == BuildStyle::Desert ? B::MudBricks : B::DarkPlanks;
    p.window = B::Air;
    Reserve* rp = nullptr;
    std::unique_ptr<Reserve> holder;
    const Shell s = houseShell(ctx, rp, holder, 11, 9, 1, p, false);
    Reserve& r = *rp;
    Blueprint& bp = ctx.bp;
    // Fachada abierta con valla.
    for (int x = s.x0 + 1; x < s.x1; ++x) {
        for (int y = 1; y <= 3; ++y) bp.set(x, y, s.z0, B::Air);
        if (std::abs(x - s.door_x) > 1) bp.set(x, 1, s.z0, B::Fence);
    }
    // Boxes con paja: vallas perpendiculares al fondo.
    for (int x = s.ix0 + 2; x <= s.ix1 - 1; x += 3) {
        for (int z = s.iz1 - 3; z <= s.iz1; ++z) place(bp, r, x, 1, z, B::Fence);
    }
    for (int x = s.ix0; x <= s.ix1; ++x) {
        if (bp.get(x, 1, s.iz1) == B::Air && x % 3 != 0) place(bp, r, x, 1, s.iz1, B::HayBale);
        if (bp.get(x, 0, s.iz1 - 1) == p.floor) bp.set(x, 0, s.iz1 - 1, B::HayBale);
    }
    place(bp, r, s.ix0, 1, s.iz0, B::Barrel);
    place(bp, r, s.ix0, 2, s.iz0, B::HayBale);
    place(bp, r, (s.ix0 + s.ix1) / 2, 3, (s.iz0 + s.iz1) / 2, B::Lantern);
    ctx.point("mercader", static_cast<float>(s.door_x), 1, static_cast<float>(s.iz0 + 1), 0, "monturas");
    ctx.point("npc", static_cast<float>(s.ix1 - 1), 1, static_cast<float>(s.iz0 + 1), 3, "mozo");
}

// --- Templo -------------------------------------------------------------------------------

void temple(BuildContext& ctx) {
    const int W = 13, D = 21, wall_h = 9;
    const int roof_layers = (W + 1) / 2;
    ctx.bp = Blueprint(W, wall_h + roof_layers + 3, D);
    Blueprint& bp = ctx.bp;
    // Cuerpo: x 1..11, z 3..20 (portico delante, z 0..2).
    const int x0 = 1, x1 = W - 2, z0 = 3, z1 = D - 1;
    const int cx = W / 2;
    for (int z = 0; z < D; ++z)
        for (int x = x0; x <= x1; ++x) bp.set(x, 0, z, (z < z0) ? B::Marble : B::Paving);
    for (int y = 1; y <= wall_h; ++y) {
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const bool wall = x == x0 || x == x1 || z == z0 || z == z1;
                if (!wall) {
                    bp.set(x, y, z, y == wall_h ? B::Marble : B::Air);
                    continue;
                }
                BlockId m = B::Marble;
                const int along = (x == x0 || x == x1) ? z - z0 : x - x0;
                if (along % 3 == 0) m = B::MarblePillar;
                if (y == wall_h || y == 1) m = B::CarvedStone;
                if (along % 3 != 0 && y >= 3 && y <= 6 && (x == x0 || x == x1)) m = B::Glass;  // ventanales
                bp.set(x, y, z, m);
            }
        }
    }
    // Portico: columnas delante.
    for (int x = x0; x <= x1; x += 2) {
        for (int y = 1; y < wall_h; ++y) bp.set(x, y, 0, B::MarblePillar);
        bp.set(x, wall_h, 0, B::CarvedStone);
    }
    for (int z = 0; z < z0; ++z)
        for (int x = x0; x <= x1; ++x) bp.set(x, wall_h, z, B::Marble);
    // Puerta grande (3 x 4).
    for (int x = cx - 1; x <= cx + 1; ++x)
        for (int y = 1; y <= 4; ++y) bp.set(x, y, z0, B::Door);
    bp.set(cx, 6, z0, B::Glass);  // rosetón
    bp.set(cx - 1, 6, z0, B::Glass);
    bp.set(cx + 1, 6, z0, B::Glass);
    // Tejado a dos aguas (cumbrera a lo largo, de delante atras).
    for (int k = 0;; ++k) {
        const int xa = k, xb = W - 1 - k;
        if (xa > xb) break;
        const int y = wall_h + 1 + k;
        for (int z = 0; z < D; ++z) {
            const BlockId t = xa >= xb - 1 ? B::GoldBlock : ctx.roof;
            bp.set(xa, y, z, t);
            bp.set(xb, y, z, t);
            if (z == 0 || z == D - 1) {
                for (int x = xa + 1; x < xb; ++x) bp.set(x, y, z, B::Marble);
            } else {
                for (int x = xa + 1; x < xb; ++x) bp.set(x, y, z, B::Air);
            }
        }
    }
    // Interior: alfombra central, bancos, altar en un estrado, velas y braseros.
    for (int z = z0 + 1; z < z1; ++z) bp.set(cx, 0, z, B::RugRed);
    for (int z = z0 + 3; z < z1 - 5; z += 2) {
        for (int x = x0 + 2; x <= x1 - 2; ++x) {
            if (std::abs(x - cx) <= 1) continue;
            bp.set(x, 1, z, B::Bench);
        }
    }
    for (int z = z1 - 4; z < z1; ++z)
        for (int x = x0 + 1; x < x1; ++x) bp.set(x, 1, z, B::CarvedStone);  // estrado
    for (int x = cx - 1; x <= cx + 1; ++x) bp.set(x, 2, z1 - 2, B::Altar);
    bp.set(cx - 1, 3, z1 - 2, B::Candles);
    bp.set(cx + 1, 3, z1 - 2, B::Candles);
    bp.set(cx, 3, z1 - 2, B::GoldBlock);
    bp.set(x0 + 1, 2, z1 - 1, B::Brazier);
    bp.set(x1 - 1, 2, z1 - 1, B::Brazier);
    for (int y = 2; y <= 5; ++y) {
        bp.set(cx - 3, y, z1, B::BannerBlue);
        bp.set(cx + 3, y, z1, B::BannerBlue);
    }
    for (int z = z0 + 3; z < z1 - 3; z += 4) {
        bp.set(x0 + 1, 1, z, B::Brazier);
        bp.set(x1 - 1, 1, z, B::Brazier);
        bp.set(cx, wall_h - 1, z, B::Lantern);
    }
    // Braseros en el portico.
    bp.set(x0 + 1, 1, 1, B::Brazier);
    bp.set(x1 - 1, 1, 1, B::Brazier);
    ctx.point("altar", static_cast<float>(cx), 3, static_cast<float>(z1 - 2), 0);
    ctx.point("npc", static_cast<float>(cx), 2, static_cast<float>(z1 - 3), 0, "sacerdote");
    ctx.point("npc", static_cast<float>(cx - 3), 1, static_cast<float>(z0 + 4), 2, "monje");
    ctx.point("mercader", static_cast<float>(x1 - 2), 2, static_cast<float>(z1 - 3), 3, "pociones");
}

// --- Pozo, fuente, puesto, granja, farola, arbol, banco ------------------------------------

void well(BuildContext& ctx) {
    // Suelo en y = 3 (el plano empieza 3 bajo tierra).
    ctx.bp = Blueprint(5, 9, 5);
    Blueprint& bp = ctx.bp;
    const int g = 3;
    for (int z = 0; z < 5; ++z) {
        for (int x = 0; x < 5; ++x) {
            const bool ring = x == 0 || x == 4 || z == 0 || z == 4;
            const bool inner = x >= 1 && x <= 3 && z >= 1 && z <= 3;
            if (ring) {
                for (int y = 0; y <= g + 1; ++y) bp.set(x, y, z, B::Cobblestone);
            } else if (inner) {
                for (int y = 0; y < g; ++y) bp.set(x, y, z, B::PoolWater);
                bp.set(x, g, z, B::Air);
                bp.set(x, g + 1, z, B::Air);
            }
        }
    }
    for (int x = 0; x < 5; x += 4) {
        for (int z = 0; z < 5; z += 4) {
            bp.set(x, g + 2, z, B::Timber);
            bp.set(x, g + 3, z, B::Timber);
        }
    }
    for (int z = 0; z < 5; ++z)
        for (int x = 0; x < 5; ++x) bp.set(x, g + 4, z, ctx.roof);
    for (int x = 1; x <= 3; ++x) bp.set(x, g + 5, 2, ctx.roof);
    bp.set(2, g + 5, 2, B::Timber);
    bp.set(2, g + 3, 2, B::Lantern);
    ctx.point("plaza", 2, g + 1, -1, 2);
}

void fountain(BuildContext& ctx, int size) {
    ctx.bp = Blueprint(size, 8, size);
    Blueprint& bp = ctx.bp;
    const int c = size / 2;
    // Suelo en y = 1 (el plano empieza 1 bajo tierra).
    for (int z = 0; z < size; ++z) {
        for (int x = 0; x < size; ++x) {
            const bool ring = x == 0 || z == 0 || x == size - 1 || z == size - 1;
            bp.set(x, 0, z, B::StoneBricks);
            if (ring) {
                bp.set(x, 1, z, B::Paving);
                bp.set(x, 2, z, B::Marble);
            } else {
                bp.set(x, 1, z, B::PoolWater);
                bp.set(x, 2, z, B::Air);
            }
        }
    }
    for (int z = c - 1; z <= c + 1; ++z)
        for (int x = c - 1; x <= c + 1; ++x) bp.set(x, 2, z, B::Marble);
    for (int y = 3; y <= 4; ++y) bp.set(c, y, c, B::MarblePillar);
    for (int z = c - 1; z <= c + 1; ++z)
        for (int x = c - 1; x <= c + 1; ++x) bp.set(x, 5, z, (x == c && z == c) ? B::PoolWater : B::Marble);
    bp.set(c, 6, c, B::GoldBlock);
    // Braseros en las esquinas del borde.
    bp.set(0, 3, 0, B::Lantern);
    bp.set(size - 1, 3, 0, B::Lantern);
    bp.set(0, 3, size - 1, B::Lantern);
    bp.set(size - 1, 3, size - 1, B::Lantern);
}

void stall(BuildContext& ctx, BlockId cloth, const std::string& shop_kind) {
    ctx.bp = Blueprint(4, 6, 3);
    Blueprint& bp = ctx.bp;
    for (int x = 0; x < 4; x += 3) {
        for (int z = 0; z < 3; z += 2) {
            for (int y = 1; y <= 3; ++y) bp.set(x, y, z, B::Timber);
        }
    }
    for (int z = 0; z < 3; ++z)
        for (int x = 0; x < 4; ++x) bp.set(x, 4, z, (x % 2 == 0) ? cloth : B::ClothWhite);
    for (int x = 0; x < 4; ++x) bp.set(x, 5, 1, (x % 2 == 0) ? cloth : B::ClothWhite);
    bp.set(1, 1, 0, B::Counter);
    bp.set(2, 1, 0, B::Counter);
    BlockId goods = B::Crate;
    if (shop_kind == "pociones") goods = B::Shelf;
    else if (shop_kind == "armas" || shop_kind == "armaduras") goods = B::WeaponRack;
    else if (shop_kind == "monturas") goods = B::HayBale;
    bp.set(1, 2, 0, shop_kind == "pociones" ? B::Candles : B::Air);
    bp.set(1, 1, 2, goods);
    bp.set(2, 1, 2, B::Barrel);
    bp.set(1, 1, 1, B::Air);
    bp.set(2, 1, 1, B::Air);
    bp.set(1, 3, 1, B::Lantern);
    ctx.point("mercader", 2, 1, 1, 0, shop_kind);
}

void farm(BuildContext& ctx, int w, int d) {
    ctx.bp = Blueprint(w, 3, d);
    Blueprint& bp = ctx.bp;
    const bool wheat = ctx.rand(3) < 0.6f;
    for (int z = 0; z < d; ++z) {
        for (int x = 0; x < w; ++x) {
            const bool ring = x == 0 || z == 0 || x == w - 1 || z == d - 1;
            if (ring) {
                bp.set(x, 1, z, (z == 0 && std::abs(x - w / 2) <= 0) ? B::Air : B::Fence);
                bp.set(x, 2, z, B::Air);
                continue;
            }
            const bool channel = z == d / 2;
            if (channel) {
                bp.set(x, 0, z, B::PoolWater);
                bp.set(x, 1, z, B::Air);
            } else if (x == 1 || x == w - 2) {
                bp.set(x, 0, z, B::DirtPath);
                bp.set(x, 1, z, B::Air);
            } else {
                bp.set(x, 0, z, B::Farmland);
                const bool row_wheat = wheat ? (x % 4 != 3) : (x % 4 == 3);
                bp.set(x, 1, z, row_wheat ? B::Wheat : B::Carrots);
            }
        }
    }
    bp.set(1, 1, d - 2, B::HayBale);
    bp.set(1, 2, d - 2, B::HayBale);
    bp.set(2, 1, d - 2, B::HayBale);
    bp.set(w - 2, 1, d - 2, B::Barrel);
    // Espantapajaros.
    bp.set(w / 2, 1, d / 2 - 1 > 0 ? d / 2 - 1 : 1, B::Fence);
    bp.set(w / 2, 2, d / 2 - 1 > 0 ? d / 2 - 1 : 1, B::HayBale);
    ctx.point("npc", static_cast<float>(w / 2), 1, 1, 2, "granjero");
}

void lampPost(BuildContext& ctx) {
    ctx.bp = Blueprint(1, 5, 1);
    ctx.bp.set(0, 0, 0, B::Cobblestone);
    for (int y = 1; y <= 3; ++y) ctx.bp.set(0, y, 0, B::Timber);
    ctx.bp.set(0, 4, 0, B::Lantern);
}

void tree(BuildContext& ctx) {
    ctx.bp = Blueprint(7, 12, 7);
    Blueprint& bp = ctx.bp;
    if (ctx.style == BuildStyle::Snow) {
        // Abeto: capas que se estrechan.
        const int h = 6 + static_cast<int>(ctx.rand(4) * 3.0f);
        for (int y = 1; y <= h; ++y) bp.set(3, y, 3, B::SpruceLog);
        for (int y = 2; y <= h + 1; ++y) {
            int r = std::max(0, 3 - (y - 2) * 3 / std::max(1, h - 1));
            if ((h - y) % 2 == 0 && r > 0) --r;
            for (int z = 3 - r; z <= 3 + r; ++z)
                for (int x = 3 - r; x <= 3 + r; ++x)
                    if (std::abs(x - 3) + std::abs(z - 3) <= r + 1 && !(x == 3 && z == 3 && y <= h)) bp.set(x, y, z, B::SpruceLeaves);
        }
        bp.set(3, h + 1, 3, B::SpruceLeaves);
        return;
    }
    if (ctx.style == BuildStyle::Beach || ctx.style == BuildStyle::Desert) {
        // Palmera: tronco y hojas que caen en cruz.
        const int h = 5 + static_cast<int>(ctx.rand(4) * 3.0f);
        if (h + 1 >= bp.h) return;
        for (int y = 1; y <= h; ++y) bp.set(3, y, 3, B::OakLog);
        bp.set(3, h + 1, 3, B::OakLeaves);
        static constexpr int kDirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
        for (int d = 0; d < 8; ++d)
            for (int i = 1; i <= (d < 4 ? 3 : 2); ++i) bp.set(3 + kDirs[d][0] * i, h + 1 - (i >= 2 ? 1 : 0), 3 + kDirs[d][1] * i, B::OakLeaves);
        return;
    }
    const int h = 4 + static_cast<int>(ctx.rand(4) * 2.0f);
    const float radius = 2.3f + ctx.rand(5) * 0.6f;
    for (int y = h - 1; y <= h + 3; ++y) {
        for (int z = 0; z < 7; ++z) {
            for (int x = 0; x < 7; ++x) {
                const float dx = static_cast<float>(x - 3), dz = static_cast<float>(z - 3), dy = static_cast<float>(y - h - 1) * 1.25f;
                const float dd = dx * dx + dz * dz + dy * dy;
                if (dd <= radius * radius && !(dd > (radius - 0.8f) * (radius - 0.8f) && ctx.rand(x, y, z) < 0.3f)) {
                    bp.set(x, y, z, B::OakLeaves);
                }
            }
        }
    }
    for (int y = 1; y <= h; ++y) bp.set(3, y, 3, B::OakLog);
    bp.set(3, 0, 3, B::Dirt);
}

void bench(BuildContext& ctx) {
    ctx.bp = Blueprint(2, 2, 1);
    ctx.bp.set(0, 1, 0, B::Bench);
    ctx.bp.set(1, 1, 0, B::Bench);
}

// --- Ciudad: muralla, torres, puerta, castillo, muelle -----------------------------------

void wallSegment(BuildContext& ctx, int length, int height) {
    // z 0 cara de fuera, 0..3 muro, 4 farolas de la cara de dentro.
    ctx.bp = Blueprint(length, height + 2, 5);
    Blueprint& bp = ctx.bp;
    for (int x = 0; x < length; ++x) {
        for (int z = 0; z < 4; ++z) {
            for (int y = 0; y < height; ++y) {
                BlockId m = B::StoneBricks;
                if (y <= 1) m = ctx.rand(x, y, z) < 0.35f ? B::MossyBricks : B::Cobblestone;
                else if (ctx.rand(x, y, z + 9) < 0.08f) m = B::CrackedBricks;
                if (y == height - 2 && z == 0) m = B::CarvedStone;
                if (y == height - 1 && z > 0) m = B::Paving;  // adarve
                bp.set(x, y, z, m);
            }
        }
        // Almenas.
        if (x % 2 == 0) bp.set(x, height, 0, B::StoneBricks);
        if (x % 8 == 4) bp.set(x, 4, 4, B::Lantern);
    }
}

void tower(BuildContext& ctx, int size, int height) {
    const int R = (size + 1) / 2;  // con el alero
    ctx.bp = Blueprint(size + 2, height + R + 5, size + 2);
    Blueprint& bp = ctx.bp;
    const int c = R;  // centro (size impar)
    for (int z = 1; z <= size; ++z) {
        for (int x = 1; x <= size; ++x) {
            const bool ring = x == 1 || x == size || z == 1 || z == size;
            bp.set(x, 0, z, B::StoneBricks);
            for (int y = 1; y < height; ++y) {
                BlockId m = B::Air;
                if (ring) {
                    m = y <= 1 ? B::Cobblestone : (ctx.rand(x, y, z) < 0.08f ? B::MossyBricks : B::StoneBricks);
                    if (y % 5 == 0) m = B::CarvedStone;
                    const bool slit = (x == c || z == c) && (y % 5 == 2 || y % 5 == 3) && y > 3;
                    if (slit) m = B::Air;
                }
                bp.set(x, y, z, m);
            }
            bp.set(x, height - 1, z, B::StoneBricks);
            if (ring && (x + z) % 2 == 0) bp.set(x, height, z, B::StoneBricks);
        }
    }
    // Tejado en piramide escalonada (con alero).
    for (int k = 0; k <= R; ++k) {
        const int y = height + 1 + k;
        const int dist = R - k;
        for (int z = 0; z < size + 2; ++z) {
            for (int x = 0; x < size + 2; ++x) {
                const int dd = std::max(std::abs(x - c), std::abs(z - c));
                if (dd == dist) bp.set(x, y, z, ctx.roof);
                else if (dd < dist) bp.set(x, y, z, B::Air);
            }
        }
    }
    bp.set(c, height + R + 2, c, B::Fence);
    bp.set(c, height + R + 3, c, B::BannerRed);
    // Puerta (hacia la ciudad: -z) y farol dentro.
    bp.set(c, 1, 1, B::Door);
    bp.set(c, 2, 1, B::Door);
    bp.set(c, 3, c, B::Lantern);
    bp.set(c, height - 2, c, B::Lantern);
    ctx.point("npc", static_cast<float>(c), 1, 0, 0, "guardia");
}

void gatehouse(BuildContext& ctx, int width, int height, int wall_thickness) {
    const int D = wall_thickness + 4;
    ctx.bp = Blueprint(width, height + 4, D);
    Blueprint& bp = ctx.bp;
    const int c = width / 2;
    for (int z = 0; z < D; ++z) {
        for (int x = 0; x < width; ++x) {
            const bool side_tower = x < 4 || x >= width - 4;
            const int top = side_tower ? height + 2 : height;
            for (int y = 0; y < top; ++y) {
                BlockId m = y <= 1 ? B::Cobblestone : B::StoneBricks;
                if (y == top - 3 && (z == 0 || z == D - 1)) m = B::CarvedStone;
                bp.set(x, y, z, m);
            }
            if ((x + z) % 2 == 0 && (x == 0 || x == width - 1 || z == 0 || z == D - 1 || x == 3 || x == width - 4)) {
                bp.set(x, top, z, B::StoneBricks);
            }
        }
    }
    // Arco de paso (5 de ancho), suelo empedrado y rastrillo levantado.
    for (int z = 0; z < D; ++z) {
        for (int x = c - 2; x <= c + 2; ++x) {
            bp.set(x, 0, z, B::Paving);
            for (int y = 1; y <= 5; ++y) bp.set(x, y, z, B::Air);
            bp.set(x, 6, z, std::abs(x - c) == 2 ? B::Air : B::StoneBricks);
        }
    }
    for (int x = c - 2; x <= c + 2; ++x) bp.set(x, 5, 1, B::IronBars);
    bp.set(c - 2, 4, 0, B::Lantern);
    bp.set(c + 2, 4, 0, B::Lantern);
    bp.set(c - 2, 4, D - 1, B::Lantern);
    bp.set(c + 2, 4, D - 1, B::Lantern);
    // Estandartes en la cara de fuera.
    for (int y = height - 5; y <= height - 2; ++y) {
        bp.set(1, y, 0, B::BannerRed);
        bp.set(width - 2, y, 0, B::BannerRed);
    }
    ctx.point("npc", static_cast<float>(c - 2), 1, 0, 0, "guardia");
    ctx.point("npc", static_cast<float>(c + 2), 1, 0, 0, "guardia");
}

void castle(BuildContext& ctx, int w, int d) {
    const int g = 6;  // suelo del patio (el plano empieza 6 bajo tierra: mazmorra)
    const int keep_h = 11;
    ctx.bp = Blueprint(w, g + 34, d);
    Blueprint& bp = ctx.bp;
    Reserve r(bp);
    const int c = w / 2;
    // Patio empedrado.
    for (int z = 0; z < d; ++z)
        for (int x = 0; x < w; ++x) {
            bp.set(x, g, z, B::Paving);
            for (int y = 0; y < g; ++y) bp.set(x, y, z, B::Stone);
        }
    // Muralla exterior (2 de grueso, 9 de alto, almenas).
    const int curtain = 9;
    for (int z = 0; z < d; ++z) {
        for (int x = 0; x < w; ++x) {
            const int edge = std::min(std::min(x, w - 1 - x), std::min(z, d - 1 - z));
            if (edge > 1) continue;
            for (int y = g + 1; y <= g + curtain; ++y) {
                BlockId m = y <= g + 1 ? B::Cobblestone : B::StoneBricks;
                if (ctx.rand(x, y, z) < 0.07f) m = B::MossyBricks;
                if (y == g + curtain - 1 && edge == 0) m = B::CarvedStone;
                bp.set(x, y, z, m);
            }
            if (edge == 0 && (x + z) % 2 == 0) bp.set(x, g + curtain + 1, z, B::StoneBricks);
        }
    }
    // Puerta del castillo (delante, -z) con estandartes.
    for (int z = 0; z <= 1; ++z)
        for (int x = c - 2; x <= c + 2; ++x)
            for (int y = g + 1; y <= g + 5; ++y) bp.set(x, y, z, B::Air);
    for (int x = c - 2; x <= c + 2; ++x) bp.set(x, g + 5, 0, B::IronBars);
    for (int y = g + 3; y <= g + 7; ++y) {
        bp.set(c - 4, y, 0, B::BannerRed);
        bp.set(c + 4, y, 0, B::BannerRed);
    }
    ctx.point("npc", static_cast<float>(c - 2), static_cast<float>(g + 1), 2, 0, "guardia");
    ctx.point("npc", static_cast<float>(c + 2), static_cast<float>(g + 1), 2, 0, "guardia");
    // Torres de las esquinas (7 x 7) con tejado en piramide.
    const int tower_h = 16;
    const int corners[4][2] = {{0, 0}, {w - 7, 0}, {0, d - 7}, {w - 7, d - 7}};
    for (const auto& corner : corners) {
        const int ox = corner[0], oz = corner[1];
        for (int z = 0; z < 7; ++z) {
            for (int x = 0; x < 7; ++x) {
                const bool ring = x == 0 || x == 6 || z == 0 || z == 6;
                for (int y = g + 1; y <= g + tower_h; ++y) {
                    BlockId m = ring ? (ctx.rand(ox + x, y, oz + z) < 0.08f ? B::MossyBricks : B::StoneBricks) : B::Air;
                    if (ring && (y - g) % 6 == 0) m = B::CarvedStone;
                    if (ring && (x == 3 || z == 3) && (y - g) % 6 == 3) m = B::Air;  // saeteras
                    if (y == g + tower_h) m = B::StoneBricks;
                    bp.set(ox + x, y, oz + z, m);
                }
            }
        }
        for (int k = 0; k <= 3; ++k) {
            const int y = g + tower_h + 1 + k;
            for (int z = 0; z < 7; ++z)
                for (int x = 0; x < 7; ++x) {
                    const int dd = std::max(std::abs(x - 3), std::abs(z - 3));
                    if (dd == 3 - k) bp.set(ox + x, y, oz + z, ctx.roof);
                    else if (dd < 3 - k) bp.set(ox + x, y, oz + z, B::Air);
                }
        }
        bp.set(ox + 3, g + tower_h + 5, oz + 3, B::Fence);
        bp.set(ox + 3, g + tower_h + 6, oz + 3, B::BannerBlue);
        bp.set(ox + 3, g + 2, oz + 3, B::Lantern);
    }

    // --- Torre del homenaje ---
    const int kx0 = 9, kx1 = w - 10;
    const int kz1 = d - 4;
    const int kz0 = std::max(10, kz1 - 18);
    const int kc = (kx0 + kx1) / 2;
    const int f2 = g + 6;          // suelo del primer piso
    const int topy = g + keep_h;   // suelo del desvan / arranque del tejado
    for (int z = kz0; z <= kz1; ++z) {
        for (int x = kx0; x <= kx1; ++x) {
            const bool wall = x == kx0 || x == kx1 || z == kz0 || z == kz1;
            bp.set(x, g, z, wall ? B::StoneBricks : B::Marble);
            for (int y = g + 1; y <= topy; ++y) {
                BlockId m = B::Air;
                if (wall) {
                    m = B::StoneBricks;
                    if (y == f2 || y == topy) m = B::CarvedStone;
                    const int along = (x == kx0 || x == kx1) ? z - kz0 : x - kx0;
                    if (along % 3 == 1 && ((y >= g + 2 && y <= g + 4) || (y >= f2 + 2 && y <= f2 + 3))) m = B::Glass;
                } else if (y == f2 || y == topy) {
                    m = B::DarkPlanks;
                }
                bp.set(x, y, z, m);
            }
        }
    }
    // Tejado a dos aguas (pizarra) a lo ancho.
    {
        const int D = kz1 - kz0 + 3;  // con alero
        for (int k = 0;; ++k) {
            const int za = kz0 - 1 + k, zb = kz1 + 1 - k;
            if (za > zb) break;
            const int y = topy + k;
            for (int x = kx0 - 1; x <= kx1 + 1; ++x) {
                const BlockId t = za >= zb - 1 ? B::CarvedStone : ctx.roof;
                bp.set(x, y, za, t);
                bp.set(x, y, zb, t);
            }
            if (k == 0) continue;
            for (int z = za + 1; z < zb; ++z)
                for (int x = kx0; x <= kx1; ++x) bp.set(x, y, z, (x == kx0 || x == kx1) ? B::StoneBricks : B::Air);
        }
        (void)D;
    }
    // Puerta grande del torreon.
    for (int x = kc - 1; x <= kc + 1; ++x)
        for (int y = g + 1; y <= g + 4; ++y) bp.set(x, y, kz0, B::Door);
    // Salon del trono: alfombra, columnas, braseros, estrado y trono.
    for (int z = kz0 + 1; z < kz1; ++z)
        for (int x = kc - 1; x <= kc + 1; ++x) bp.set(x, g, z, B::RugRed);
    for (int z = kz0 + 2; z < kz1 - 4; z += 4) {
        for (int y = g + 1; y < f2; ++y) {
            bp.set(kx0 + 3, y, z, B::MarblePillar);
            bp.set(kx1 - 3, y, z, B::MarblePillar);
        }
        bp.set(kx0 + 3, g + 1, z + 2, B::Brazier);
        bp.set(kx1 - 3, g + 1, z + 2, B::Brazier);
        bp.set(kc, f2 - 1, z, B::Lantern);
    }
    for (int z = kz1 - 4; z < kz1; ++z)
        for (int x = kc - 4; x <= kc + 4; ++x) bp.set(x, g + 1, z, B::CarvedStone);
    for (int z = kz1 - 2; z < kz1; ++z)
        for (int x = kc - 2; x <= kc + 2; ++x) bp.set(x, g + 2, z, B::Marble);
    bp.set(kc, g + 3, kz1 - 1, B::Throne);
    bp.set(kc, g + 4, kz1 - 1, B::Throne);
    bp.set(kc - 2, g + 3, kz1 - 1, B::GoldBlock);
    bp.set(kc + 2, g + 3, kz1 - 1, B::GoldBlock);
    bp.set(kc - 2, g + 4, kz1 - 1, B::Candles);
    bp.set(kc + 2, g + 4, kz1 - 1, B::Candles);
    for (int y = g + 2; y <= g + 5; ++y) {
        bp.set(kc - 4, y, kz1, B::BannerRed);
        bp.set(kc + 4, y, kz1, B::BannerRed);
        bp.set(kc, y + 1, kz1, B::BannerBlue);
    }
    ctx.point("npc", static_cast<float>(kc), static_cast<float>(g + 3), static_cast<float>(kz1 - 2), 0, "rey");
    ctx.point("npc", static_cast<float>(kc - 2), static_cast<float>(g + 2), static_cast<float>(kz1 - 3), 0, "consejero");
    for (int i = 0; i < 4; ++i) {
        const int z = kz0 + 3 + i * 3;
        if (z >= kz1 - 4) break;
        ctx.point("npc", static_cast<float>(kc + (i % 2 ? 2 : -2)), static_cast<float>(g + 1), static_cast<float>(z), i % 2 ? 3 : 1, "guardia");
    }
    // Escalera al primer piso (muro derecho) y a la mazmorra (muro izquierdo).
    stairsZ(bp, r, kx1 - 1, g, kz0 + 2, 6, B::DarkPlanks);
    for (int z = kz0 + 2; z < kz0 + 7; ++z) bp.set(kx1 - 2, f2 + 1, z, B::Fence);
    // Bajada: escalones desde el sotano (suelo 0) hasta el salon (g).
    stairsZ(bp, r, kx0 + 1, 0, kz0 + 2, g, B::StoneBricks);
    // Sotano: calabozos.
    for (int z = kz0; z <= kz1; ++z) {
        for (int x = kx0; x <= kx1; ++x) {
            const bool wall = x == kx0 || x == kx1 || z == kz0 || z == kz1;
            bp.set(x, 0, z, B::DungeonBricks);
            for (int y = 1; y < g; ++y) {
                if (wall) bp.set(x, y, z, ctx.rand(x, y, z) < 0.3f ? B::MossyBricks : B::DungeonBricks);
                else if (bp.get(x, y, z) == B::Stone) bp.set(x, y, z, B::Air);
            }
        }
    }
    stairsZ(bp, r, kx0 + 1, 0, kz0 + 2, g, B::StoneBricks);  // otra vez: el sotano la habia vaciado
    for (int x = kx0 + 4; x < kx1; ++x) {
        const bool post = (x - kx0) % 4 == 0;
        for (int y = 1; y <= 3; ++y) bp.set(x, y, kz1 - 4, post ? B::DungeonBricks : B::IronBars);
        if (!post) {
            if ((x - kx0) % 4 == 2) bp.set(x, 1, kz1 - 2, ctx.rand(x, 3) < 0.5f ? B::Bones : B::HayBale);
        } else {
            for (int z = kz1 - 3; z < kz1; ++z)
                for (int y = 1; y <= 3; ++y) bp.set(x, y, z, B::DungeonBricks);
        }
    }
    for (int z = kz0 + 3; z < kz1 - 5; z += 4) bp.set(kc, 4, z, B::Lantern);
    bp.set(kx1 - 1, 1, kz0 + 1, B::Table);
    bp.set(kx1 - 1, 2, kz0 + 1, B::Candles);
    bp.set(kx1 - 2, 1, kz0 + 1, B::Chair);
    ctx.point("npc", static_cast<float>(kx1 - 2), 1, static_cast<float>(kz0 + 2), 0, "carcelero");
    ctx.point("npc", static_cast<float>(kx0 + 6), 1, static_cast<float>(kz1 - 2), 0, "prisionero");

    // Primer piso: cuatro salas (biblioteca, armeria, comedor y dormitorio real).
    const int y1 = f2 + 1;
    const int kzm = (kz0 + kz1) / 2;
    for (int y = y1; y < topy; ++y) {
        for (int z = kz0 + 1; z < kz1; ++z) bp.set(kc, y, z, (y == y1 + 2 && (z == kz0 + 3 || z == kz1 - 3)) ? B::Painting : B::StoneBricks);
        for (int x = kx0 + 1; x < kx1; ++x) bp.set(x, y, kzm, (y == y1 + 2 && (x == kx0 + 3 || x == kx1 - 3)) ? B::Painting : B::StoneBricks);
    }
    // Puertas entre salas.
    const int doors[4][2] = {{kc, kzm - 3}, {kc, kzm + 3}, {kc - 3, kzm}, {kc + 3, kzm}};
    for (const auto& dd : doors) {
        bp.set(dd[0], y1, dd[1], B::Door);
        bp.set(dd[0], y1 + 1, dd[1], B::Door);
        r.markColumn(dd[0] + (dd[0] == kc ? 1 : 0), y1, y1 + 1, dd[1] + (dd[0] == kc ? 0 : 1));
        r.markColumn(dd[0] - (dd[0] == kc ? 1 : 0), y1, y1 + 1, dd[1] - (dd[0] == kc ? 0 : 1));
    }
    // Biblioteca (delante izquierda).
    for (int z = kz0 + 1; z < kzm; ++z) {
        if (place(bp, r, kx0 + 1, y1, z, B::Bookshelf)) place(bp, r, kx0 + 1, y1 + 1, z, B::Bookshelf);
    }
    for (int x = kx0 + 2; x < kc; ++x) {
        if (place(bp, r, x, y1, kz0 + 1, B::Bookshelf)) place(bp, r, x, y1 + 1, kz0 + 1, B::Bookshelf);
    }
    if (place(bp, r, (kx0 + kc) / 2, y1, kzm - 2, B::Table)) bp.set((kx0 + kc) / 2, y1 + 1, kzm - 2, B::Candles);
    place(bp, r, (kx0 + kc) / 2 - 1, y1, kzm - 2, B::Chair);
    place(bp, r, (kx0 + kc) / 2, y1 + 2, kz0 + 4, B::Lantern);
    ctx.point("npc", static_cast<float>((kx0 + kc) / 2 + 1), static_cast<float>(y1), static_cast<float>(kzm - 2), 3, "bibliotecario");
    // Armeria (delante derecha; la escalera llega aqui).
    for (int x = kc + 1; x < kx1 - 1; ++x) {
        if (place(bp, r, x, y1, kz0 + 1, B::WeaponRack)) place(bp, r, x, y1 + 1, kz0 + 1, B::WeaponRack);
    }
    place(bp, r, kc + 2, y1, kzm - 1, B::Chest);
    place(bp, r, kc + 3, y1, kzm - 1, B::Anvil);
    place(bp, r, kc + 1, y1, kzm - 1, B::Barrel);
    place(bp, r, (kc + kx1) / 2, y1 + 2, kz0 + 4, B::Lantern);
    ctx.point("npc", static_cast<float>(kc + 3), static_cast<float>(y1), static_cast<float>(kzm - 2), 2, "armero");
    ctx.point("mercader", static_cast<float>(kc + 2), static_cast<float>(y1), static_cast<float>(kz0 + 2), 0, "armaduras");
    // Comedor (fondo izquierda): mesa larga con sillas, horno y barriles.
    const int dz = (kzm + kz1) / 2;
    for (int x = kx0 + 3; x < kc - 1; ++x) {
        if (place(bp, r, x, y1, dz, B::Table)) {
            if (x % 2) bp.set(x, y1 + 1, dz, B::Candles);
            place(bp, r, x, y1, dz - 1, B::Chair);
            place(bp, r, x, y1, dz + 1, B::Chair);
        }
    }
    place(bp, r, kx0 + 1, y1, kz1 - 1, B::Oven);
    place(bp, r, kx0 + 1, y1, kz1 - 2, B::Barrel);
    place(bp, r, kx0 + 1, y1, kzm + 1, B::Barrel);
    place(bp, r, (kx0 + kc) / 2, y1 + 2, dz, B::Lantern);
    ctx.point("npc", static_cast<float>(kx0 + 2), static_cast<float>(y1), static_cast<float>(kz1 - 1), 1, "cocinero");
    // Dormitorio real (fondo derecha).
    for (int z = kzm + 2; z < kz1 - 1; ++z)
        for (int x = kc + 2; x < kx1 - 1; ++x) bp.set(x, f2, z, B::RugBlue);
    if (place(bp, r, kx1 - 1, y1, kz1 - 1, B::Bed)) {
        place(bp, r, kx1 - 2, y1, kz1 - 1, B::Bed);
        place(bp, r, kx1 - 1, y1, kz1 - 2, B::Bed);
        place(bp, r, kx1 - 2, y1, kz1 - 2, B::Bed);
        ctx.point("cama", static_cast<float>(kx1 - 1), static_cast<float>(y1 + 1), static_cast<float>(kz1 - 1), 0);
    }
    place(bp, r, kx1 - 3, y1, kz1 - 1, B::Table);
    bp.set(kx1 - 3, y1 + 1, kz1 - 1, B::Candles);
    place(bp, r, kc + 1, y1, kz1 - 1, B::Chest);
    if (place(bp, r, kc + 1, y1, kzm + 1, B::Bookshelf)) place(bp, r, kc + 1, y1 + 1, kzm + 1, B::Bookshelf);
    place(bp, r, kx1 - 1, y1, kzm + 1, B::GoldBlock);
    place(bp, r, (kc + kx1) / 2, y1 + 2, dz, B::Lantern);
    ctx.point("cofre", static_cast<float>(kc + 1), static_cast<float>(y1 + 1), static_cast<float>(kz1 - 1), 0, "3");
    ctx.point("npc", static_cast<float>(kc + 3), static_cast<float>(y1), static_cast<float>(kz1 - 3), 1, "reina");

    // Patio: braseros a los lados del camino, bancos y un arbol en cada lado.
    for (int z = 4; z < kz0 - 1; z += 4) {
        bp.set(c - 3, g + 1, z, B::Brazier);
        bp.set(c + 3, g + 1, z, B::Brazier);
    }
    for (int z = 2; z < kz0; ++z)
        for (int x = c - 1; x <= c + 1; ++x) bp.set(x, g, z, B::CarvedStone);
    for (int x = 9; x < c - 4; x += 3) {
        bp.set(x, g + 1, 4, B::HayBale);
        if (x % 2) bp.set(x, g + 1, 5, B::Barrel);
    }
    // Establo y cuartel apoyados en la muralla (tejadillos).
    for (int x = c + 5; x < w - 9; ++x) {
        bp.set(x, g + 4, 2, ctx.roof);
        bp.set(x, g + 4, 3, ctx.roof);
        if (x % 3 == 0) bp.set(x, g + 1, 3, B::WeaponRack);
        else bp.set(x, g + 1, 2, B::Crate);
    }
    ctx.point("npc", static_cast<float>(c + 6), static_cast<float>(g + 1), 4, 0, "capitan");
    // Torreones en las esquinas del homenaje (5 x 5, mas altos que el tejado) y
    // un torreon central con aguja.
    const auto turret = [&](int tx, int tz, int size, int top, BlockId roof) {
        const int half = size / 2;
        for (int z = tz - half; z <= tz + half; ++z)
            for (int x = tx - half; x <= tx + half; ++x) {
                const bool ring = std::abs(x - tx) == half || std::abs(z - tz) == half;
                const bool inside_keep = x > kx0 && x < kx1 && z > kz0 && z < kz1;
                for (int y = g + 1; y <= top; ++y) {
                    if (inside_keep && y <= topy) continue;  // las salas de dentro no se tocan
                    if (ring) bp.set(x, y, z, (y - g) % 6 == 0 ? B::CarvedStone : B::StoneBricks);
                    else if (y > topy) bp.set(x, y, z, y == top ? B::StoneBricks : B::Air);
                }
                if (ring && (x + z) % 2 == 0) bp.set(x, top + 1, z, B::StoneBricks);
                if (ring && (std::abs(x - tx) == 0 || std::abs(z - tz) == 0) && top - 3 > topy) bp.set(x, top - 3, z, B::Glass);
            }
        for (int k = 0; k <= half + 1; ++k) {
            const int dist = half + 1 - k;
            for (int z = tz - half - 1; z <= tz + half + 1; ++z)
                for (int x = tx - half - 1; x <= tx + half + 1; ++x) {
                    const int dd = std::max(std::abs(x - tx), std::abs(z - tz));
                    if (dd == dist) bp.set(x, top + 2 + k, z, roof);
                    else if (dd < dist) bp.set(x, top + 2 + k, z, B::Air);
                }
        }
        bp.set(tx, top + half + 4, tz, B::Fence);
        bp.set(tx, top + half + 5, tz, B::BannerRed);
    };
    turret(kx0, kz0, 5, topy + 6, ctx.roof);
    turret(kx1, kz0, 5, topy + 6, ctx.roof);
    turret(kx0, kz1, 5, topy + 6, ctx.roof);
    turret(kx1, kz1, 5, topy + 6, ctx.roof);
    // Estandartes grandes en la fachada del homenaje.
    for (int y = g + 2; y <= f2 + 3; ++y) {
        bp.set(kc - 4, y, kz0, B::BannerRed);
        bp.set(kc + 4, y, kz0, B::BannerRed);
    }
    bp.set(kc - 2, g + 5, kz0 - 1, B::Lantern);
    bp.set(kc + 2, g + 5, kz0 - 1, B::Lantern);
    // Jardines del patio: setos (arbustos), flores y dos arboles.
    for (int z = 3; z < kz0 - 2; ++z) {
        for (int x = 3; x < w - 3; ++x) {
            if (std::abs(x - c) <= 4) continue;
            const bool in_tower = (x < 8 || x > w - 9) && z < 8;
            if (in_tower || bp.get(x, g + 1, z) != kKeep) continue;
            const bool hedge = (z == 3 || z == kz0 - 3) && x > 8 && x < w - 9;
            if (hedge) bp.set(x, g + 1, z, B::OakLeaves);
            else if ((x * 7 + z * 3) % 11 == 0) bp.set(x, g + 1, z, (x + z) % 2 ? B::RedFlower : B::YellowFlower);
        }
    }
    // El resto del patio, despejado (sin la hierba del terreno).
    for (int z = 2; z < d - 2; ++z)
        for (int x = 2; x < w - 2; ++x)
            for (int y = g + 1; y <= g + 4; ++y)
                if (bp.get(x, y, z) == kKeep) bp.set(x, y, z, B::Air);
    (void)r;
}

void pier(BuildContext& ctx, int length, int sea) {
    // y 0 = sea - 8; tablero a sea + 1. Hacia +z (mar adentro); la fachada (-z) toca tierra.
    const int deck = 9;
    ctx.bp = Blueprint(5, deck + 4, length);
    Blueprint& bp = ctx.bp;
    for (int z = 0; z < length; ++z) {
        for (int x = 0; x < 5; ++x) {
            bp.set(x, deck, z, B::OakPlanks);
            for (int y = deck + 1; y <= deck + 3; ++y) bp.set(x, y, z, B::Air);
        }
        if (z % 4 == 0) {
            for (int y = 0; y < deck; ++y) {
                bp.set(0, y, z, B::OakLog);
                bp.set(4, y, z, B::OakLog);
            }
        }
        if (z > 1) {
            bp.set(0, deck + 1, z, B::Fence);
            bp.set(4, deck + 1, z, B::Fence);
        }
        if (z % 8 == 4 || z == length - 1) {
            bp.set(0, deck + 2, z, B::Lantern);
            bp.set(4, deck + 2, z, B::Lantern);
        }
    }
    for (int x = 1; x < 4; ++x) bp.set(x, deck + 1, length - 1, B::Fence);
    bp.set(1, deck + 1, length - 2, B::Barrel);
    bp.set(3, deck + 1, length - 3, B::Crate);
    ctx.point("npc", 2, static_cast<float>(deck + 1), static_cast<float>(length - 3), 2, "pescador");
    ctx.point("npc", 2, static_cast<float>(deck + 1), static_cast<float>(length / 2), 3, "marinero");
    (void)sea;
}

// --- Torre de mago ------------------------------------------------------------------------

void wizardTower(BuildContext& ctx, int floors) {
    const int R = 5;           // radio del muro
    const int S = 2 * R + 5;   // con el alero del tejado
    const int c = S / 2;
    const int fh = 6;
    const int top = floors * fh;
    const int cone = R + 4;
    ctx.bp = Blueprint(S, top + cone + 5, S);
    Blueprint& bp = ctx.bp;
    Reserve r(bp);
    const auto dist = [&](int x, int z) { return std::sqrt(static_cast<float>((x - c) * (x - c) + (z - c) * (z - c))); };
    for (int z = 0; z < S; ++z) {
        for (int x = 0; x < S; ++x) {
            const float dd = dist(x, z);
            if (dd > R + 0.5f) continue;
            const bool wall = dd > R - 0.5f;
            bp.set(x, 0, z, wall ? B::Cobblestone : B::DarkPlanks);
            for (int y = 1; y <= top; ++y) {
                BlockId m = B::Air;
                if (wall) {
                    m = y <= 2 ? (ctx.rand(x, y, z) < 0.4f ? B::MossyBricks : B::Cobblestone) : B::StoneBricks;
                    if (y % fh == 0) m = B::CarvedStone;
                    if (ctx.rand(x, y, z + 3) < 0.05f) m = B::RuneStone;
                    // Ventanas en cruz, giradas en cada piso.
                    const int f = y / fh;
                    const int ly = y % fh;
                    const bool card = (f % 2 == 0) ? (x == c || z == c) : (std::abs(x - c) == std::abs(z - c) || std::abs(std::abs(x - c) - std::abs(z - c)) == 1);
                    if (card && (ly == 3 || ly == 4) && f >= 0 && !(f == 0 && z < c)) m = B::Glass;
                } else if (y % fh == 0) {
                    m = B::DarkPlanks;  // suelo del piso de arriba
                }
                bp.set(x, y, z, m);
            }
        }
    }
    // Puerta (delante, -z).
    bp.set(c, 1, c - R, B::Door);
    bp.set(c, 2, c - R, B::Door);
    bp.set(c, 0, c - R - 1, B::CarvedStone);
    bp.set(c - 1, 3, c - R - 1, B::Lantern);
    bp.set(c + 1, 3, c - R - 1, B::Lantern);
    // Escalera de caracol alrededor de una columna central.
    static constexpr int kRing[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}};
    for (int y = 1; y <= top + 1; ++y) bp.set(c, y, c, B::CarvedStone);
    for (int y = 1; y <= top; ++y) {
        for (int i = 0; i < 8; ++i) {
            const int x = c + kRing[i][0], z = c + kRing[i][1];
            const bool step = ((y - 1) % 8) == i && y <= top - fh + 1 + 7;
            bp.set(x, y, z, step ? B::DarkPlanks : B::Air);
            r.mark(x, y, z);
            r.mark(x, y + 1, z);
            r.mark(x, y + 2, z);
        }
    }
    // Pisos: muebles en el anillo entre la escalera y el muro.
    const auto ring_place = [&](int f, float angle, float radius, BlockId id, int height = 1) {
        const int x = c + static_cast<int>(std::lround(std::cos(angle) * radius));
        const int z = c + static_cast<int>(std::lround(std::sin(angle) * radius));
        bool ok = false;
        for (int k = 0; k < height; ++k) ok = place(bp, r, x, f * fh + 1 + k, z, id) || ok;
        return ok;
    };
    const float kPi = 3.14159265f;
    for (int f = 0; f < floors; ++f) {
        const int y = f * fh + 1;
        const bool last = f == floors - 1;
        // Farol en el techo de cada piso.
        place(bp, r, c + 2, y + 4, c, B::Lantern);
        place(bp, r, c - 2, y + 4, c, B::Lantern);
        if (f == 0) {
            // Entrada: estanterias de pociones, mostrador, alfombra.
            for (int i = 0; i < 5; ++i) ring_place(f, kPi * (0.15f + 0.17f * static_cast<float>(i)), 4.0f, B::Shelf, 2);
            ring_place(f, kPi * 1.25f, 3.0f, B::Counter);
            ring_place(f, kPi * 1.75f, 3.0f, B::Barrel);
            ctx.point("mercader", static_cast<float>(c), 1, static_cast<float>(c + 3), 0, "pociones");
        } else if (last) {
            // Cima: altar, oro, cofre, cama y el mago.
            ring_place(f, kPi * 0.5f, 4.0f, B::Altar);
            bp.set(c, y + 1, c + 4, B::Candles);
            ring_place(f, kPi * 0.3f, 4.0f, B::GoldBlock);
            ring_place(f, kPi * 0.7f, 4.0f, B::RuneStone);
            ring_place(f, kPi * 1.0f, 4.0f, B::Bed);
            ring_place(f, kPi * 1.1f, 4.0f, B::Bed);
            ring_place(f, kPi * 0.0f, 4.0f, B::Chest);
            ring_place(f, kPi * 1.5f, 4.0f, B::Bookshelf, 2);
            ctx.point("npc", static_cast<float>(c), static_cast<float>(y), static_cast<float>(c + 3), 2, "mago");
            ctx.point("cofre", static_cast<float>(c + 4), static_cast<float>(y + 1), static_cast<float>(c), 1, "3");
            ctx.point("altar", static_cast<float>(c), static_cast<float>(y + 1), static_cast<float>(c + 4), 2);
        } else if (f % 2 == 1) {
            // Biblioteca.
            for (int i = 0; i < 9; ++i) ring_place(f, kPi * (0.1f + 0.2f * static_cast<float>(i)), 4.0f, B::Bookshelf, 2);
            if (ring_place(f, kPi * 0.5f, 2.6f, B::Table)) bp.set(c, y + 1, c + 3, B::Candles);
            ring_place(f, kPi * 0.75f, 2.6f, B::Chair);
            ctx.point("npc", static_cast<float>(c + 2), static_cast<float>(y), static_cast<float>(c + 2), 0, "aprendiz");
        } else {
            // Alquimia.
            for (int i = 0; i < 4; ++i) ring_place(f, kPi * (0.2f + 0.3f * static_cast<float>(i)), 4.0f, B::Shelf, 2);
            ring_place(f, kPi * 1.3f, 4.0f, B::Oven);
            ring_place(f, kPi * 1.5f, 4.0f, B::Barrel);
            ring_place(f, kPi * 1.7f, 4.0f, B::Crate);
            if (ring_place(f, kPi * 0.6f, 2.6f, B::Table)) bp.set(c - 1, y + 1, c + 3, B::Candles);
        }
    }
    // Tejado conico violeta con alero y una aguja.
    for (int k = 0; k < cone; ++k) {
        const int y = top + 1 + k;
        const float rr = static_cast<float>(R + 2) - static_cast<float>(k) * 0.9f;
        if (rr < 0.3f) break;
        for (int z = 0; z < S; ++z) {
            for (int x = 0; x < S; ++x) {
                const float dd = dist(x, z);
                if (dd <= rr && dd > rr - 1.2f) bp.set(x, y, z, ctx.roof);
                else if (dd <= rr - 1.2f) bp.set(x, y, z, B::Air);
            }
        }
    }
    int peak = top + 1;
    for (int y = top + 1; y < bp.h; ++y)
        if (bp.get(c, y, c) == ctx.roof) peak = y;
    bp.set(c, peak + 1, c, B::GoldBlock);
    bp.set(c, peak + 2, c, B::RuneStone);
}

// --- Ruinas --------------------------------------------------------------------------------

void ruin(BuildContext& ctx, int size) {
    ctx.bp = Blueprint(size, 8, size);
    Blueprint& bp = ctx.bp;
    const auto stone = [&](int x, int y, int z) {
        const float r = ctx.rand(x, y, z);
        return r < 0.35f ? B::MossyBricks : (r < 0.5f ? B::CrackedBricks : (r < 0.7f ? B::Cobblestone : B::StoneBricks));
    };
    // Dos recintos rectangulares rotos.
    const int rooms = 2;
    for (int i = 0; i < rooms; ++i) {
        const int x0 = i == 0 ? 1 : size / 3, z0 = i == 0 ? 1 : size / 3;
        const int x1 = i == 0 ? size * 2 / 3 : size - 2, z1 = i == 0 ? size * 2 / 3 : size - 2;
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const bool wall = x == x0 || x == x1 || z == z0 || z == z1;
                if (!wall) {
                    if (ctx.rand(x, 0, z) < 0.55f) bp.set(x, 0, z, stone(x, 0, z));
                    continue;
                }
                const float hn = ctx.rand(x / 3, 1, z / 3) * 0.7f + ctx.rand(x, 2, z) * 0.3f;
                const int h = static_cast<int>(hn * 6.0f);
                const bool gap = (x == (x0 + x1) / 2 || z == (z0 + z1) / 2) && h < 4;
                bp.set(x, 0, z, stone(x, 0, z));
                for (int y = 1; y <= h && !gap; ++y) bp.set(x, y, z, (y == 2 || y == 3) && (x + z) % 3 == 0 ? B::Air : stone(x, y, z));
                if (h >= 4 && ctx.rand(x, 5, z) < 0.3f) bp.set(x, h + 1, z, B::OakLeaves);
            }
        }
    }
    // Columnas, una caida, un altar roto con cofre.
    const int c = size / 2;
    for (int i = 0; i < 4; ++i) {
        const int x = i % 2 ? c + 3 : c - 3, z = i < 2 ? c - 3 : c + 3;
        const int h = 2 + static_cast<int>(ctx.rand(i, 7) * 4.0f);
        for (int y = 1; y <= h; ++y) bp.set(x, y, z, B::CarvedStone);
    }
    for (int x = c - 1; x <= c + 3; ++x) bp.set(x, 1, c + 5 < size ? c + 5 : size - 1, B::CarvedStone);
    bp.set(c, 1, c, B::Altar);
    bp.set(c + 1, 1, c, B::Chest);
    bp.set(c - 1, 1, c, B::Candles);
    bp.set(c, 1, c - 1, B::Bones);
    bp.set(c - 2, 1, c + 1, B::Cobweb);
    ctx.point("cofre", static_cast<float>(c + 1), 2, static_cast<float>(c), 0, ctx.rand(9) < 0.3f ? "2" : "1");
    ctx.point("spawner", static_cast<float>(c), 1, static_cast<float>(c - 2), 0, "");
    ctx.point("altar", static_cast<float>(c), 2, static_cast<float>(c), 0);
}

// --- Portal --------------------------------------------------------------------------------

void portal(BuildContext& ctx, const std::string& dimension) {
    const int S = 13;
    ctx.bp = Blueprint(S, 11, S);
    Blueprint& bp = ctx.bp;
    const int c = S / 2;
    const BlockId frame = B::Obsidian;
    const BlockId inner = blockId("portal");  // del agente de dimensiones (si existe)
    for (int z = 0; z < S; ++z) {
        for (int x = 0; x < S; ++x) {
            const float dd = std::hypot(static_cast<float>(x - c), static_cast<float>(z - c));
            if (dd > c + 0.3f) continue;
            bp.set(x, 0, z, dd > c - 1.0f ? B::RuneStone : (static_cast<int>(dd) % 2 ? B::CarvedStone : B::StoneBricks));
            for (int y = 1; y <= 9; ++y) bp.set(x, y, z, B::Air);
        }
    }
    // Arco (5 x 7) de obsidiana con runas en las esquinas.
    for (int y = 1; y <= 7; ++y) {
        for (int x = c - 2; x <= c + 2; ++x) {
            const bool border = x == c - 2 || x == c + 2 || y == 1 || y == 7;
            if (border) bp.set(x, y, c, ((x == c - 2 || x == c + 2) && (y == 1 || y == 7)) ? B::RuneStone : frame);
            else bp.set(x, y, c, inner != B::Air ? inner : B::Air);
        }
    }
    // Pilares con braseros.
    const int pillars[4][2] = {{c - 4, c - 4}, {c + 4, c - 4}, {c - 4, c + 4}, {c + 4, c + 4}};
    for (const auto& p : pillars) {
        for (int y = 1; y <= 3; ++y) bp.set(p[0], y, p[1], B::CarvedStone);
        bp.set(p[0], 4, p[1], B::Brazier);
    }
    ctx.point("portal", static_cast<float>(c), 2, static_cast<float>(c), 0, dimension);
    ctx.point("npc", static_cast<float>(c + 2), 1, static_cast<float>(c - 3), 0, "guardian");
}

// --- Mazmorras ------------------------------------------------------------------------------

void dungeonRoom(BuildContext& ctx, int iw, int ih, int id, RoomKind kind, int level, const std::string& spawn,
                 const std::string& boss, int shaft) {
    const int W = iw + 2, H = ih + 2, D = id + 2;
    ctx.bp = Blueprint(W, H, D);
    Blueprint& bp = ctx.bp;
    const int cx = W / 2, cz = D / 2;
    const auto wallBlock = [&](int x, int y, int z) {
        const float r = ctx.rand(x, y, z);
        return r < 0.22f ? B::MossyBricks : (r < 0.32f ? B::CrackedBricks : B::DungeonBricks);
    };
    // shaft: 1 escalera que baja desde arriba (atraviesa la sala), 2 escalera que baja desde el suelo de la sala.
    const bool shaft_here = shaft != 0;
    const auto inShaft = [&](int x, int z, int y) { return shaft_here && std::abs(x - cx) <= 2 && std::abs(z - cz) <= 2 && (shaft != 2 || y <= 1); };
    const auto onPath = [&](int x, int z) { return std::abs(x - cx) <= 1 || std::abs(z - cz) <= 1; };
    const auto nearShaft = [&](int x, int z) { return shaft_here && std::abs(x - cx) <= 3 && std::abs(z - cz) <= 3; };
    for (int z = 0; z < D; ++z) {
        for (int x = 0; x < W; ++x) {
            const bool wall = x == 0 || z == 0 || x == W - 1 || z == D - 1;
            for (int y = 0; y < H; ++y) {
                if (inShaft(x, z, y) && !wall) continue;  // el pozo de la escalera decide
                BlockId m;
                if (wall || y == H - 1) m = wallBlock(x, y, z);
                else if (y == 0) m = ctx.rand(x, 0, z) < 0.25f ? B::MossyBricks : ((x + z) % 2 ? B::StoneBricks : B::DungeonBricks);
                else m = B::Air;
                bp.set(x, y, z, m);
            }
        }
    }
    // Columnas de piedra tallada (salas grandes) y braseros en las esquinas.
    const auto free_cell = [&](int x, int z) { return x >= 1 && z >= 1 && x <= iw && z <= id && !onPath(x, z) && !nearShaft(x, z); };
    const auto put = [&](int x, int y, int z, BlockId b) {
        if (free_cell(x, z) && bp.get(x, y, z) == B::Air) bp.set(x, y, z, b);
    };
    if (iw >= 9 && id >= 9 && kind != RoomKind::Lava && kind != RoomKind::Flooded) {
        const int px[2] = {3, iw - 2}, pz[2] = {3, id - 2};
        for (int a : px)
            for (int b : pz)
                for (int y = 1; y <= ih; ++y) put(a, y, b, B::CarvedStone);
    }
    put(1, 1, 1, B::Brazier);
    put(iw, 1, 1, B::Brazier);
    put(1, 1, id, B::Brazier);
    put(iw, 1, id, B::Brazier);
    // Telaranas en las esquinas del techo y huesos sueltos.
    for (int i = 0; i < 6; ++i) {
        const int x = 1 + static_cast<int>(ctx.rand(i, 1) * static_cast<float>(iw));
        const int z = 1 + static_cast<int>(ctx.rand(i, 2) * static_cast<float>(id));
        put(x, ih, z, B::Cobweb);
        if (i < 3) put(x, 1, z, B::Bones);
    }
    std::string chest_quality;
    switch (kind) {
        case RoomKind::Library:
            for (int z = 1; z <= id; ++z)
                for (int x = 1; x <= iw; ++x) {
                    const bool edge = x == 1 || x == iw || z == 1 || z == id;
                    if (!edge) continue;
                    put(x, 1, z, B::Bookshelf);
                    if (ih >= 4) put(x, 2, z, ctx.rand(x, 3, z) < 0.8f ? B::Bookshelf : B::Cobweb);
                }
            for (int i = 0; i < 2; ++i) {
                const int x = i ? iw - 3 : 4, z = id / 2 - 2 > 2 ? id / 2 - 2 : 3;
                put(x, 1, z, B::Table);
                put(x, 2, z, B::Candles);
                put(x + 1, 1, z, B::Chair);
            }
            put(iw - 2, 1, id - 1, B::Chest);
            chest_quality = "2";
            break;
        case RoomKind::Crypt:
            for (int z = 3; z <= id - 2; z += 3) {
                for (int x = 2; x <= iw - 1; x += 3) {
                    put(x, 1, z, B::Coffin);
                    put(x, 1, z + 1, B::Coffin);
                    if (ctx.rand(x, 4, z) < 0.4f) put(x, 2, z, B::Candles);
                }
            }
            break;
        case RoomKind::Prison:
            // Celdas en las cuatro esquinas: barrotes hacia el centro.
            for (int z = 1; z <= id; ++z)
                for (int x = 1; x <= iw; ++x) {
                    const int qx = x <= cx ? x : W - 1 - x, qz = z <= cz ? z : D - 1 - z;
                    if (onPath(x, z)) continue;
                    if (qx == 3 || qz == 3) {
                        if (qx <= 3 && qz <= 3) {
                            for (int y = 1; y <= std::min(ih, 3); ++y) put(x, y, z, B::IronBars);
                        }
                    } else if (qx < 3 && qz < 3 && ctx.rand(x, 5, z) < 0.35f) {
                        put(x, 1, z, ctx.rand(x, 6, z) < 0.5f ? B::Bones : B::HayBale);
                    }
                }
            break;
        case RoomKind::Lava:
        case RoomKind::Flooded: {
            const BlockId liquid = kind == RoomKind::Lava ? B::Lava : B::PoolWater;
            for (int z = 1; z <= id; ++z)
                for (int x = 1; x <= iw; ++x) {
                    if (onPath(x, z) || nearShaft(x, z)) continue;
                    bp.set(x, 0, z, liquid);
                    bp.set(x, 1, z, B::Air);
                }
            // Puentes con pretil (bloques de piedra tallada a los lados del camino).
            for (int z = 1; z <= id; ++z)
                for (int x = 1; x <= iw; ++x) {
                    if (!onPath(x, z) || nearShaft(x, z)) continue;
                    const bool rail_x = std::abs(x - cx) == 1 && std::abs(z - cz) > 1;
                    const bool rail_z = std::abs(z - cz) == 1 && std::abs(x - cx) > 1;
                    if ((rail_x || rail_z) && (x + z) % 3 == 0) bp.set(x, 1, z, B::CarvedStone);
                }
            break;
        }
        case RoomKind::Trap:
            for (int z = 1; z <= id; ++z)
                for (int x = 1; x <= iw; ++x) {
                    if (nearShaft(x, z)) continue;
                    if ((x + 2 * z) % 4 == 0 && bp.get(x, 1, z) == B::Air) bp.set(x, 1, z, B::Spikes);
                }
            ctx.point("trampa", static_cast<float>(cx), 1, static_cast<float>(cz), 0, "pinchos");
            break;
        case RoomKind::Treasure:
            for (int i = 0; i < 4; ++i) {
                const int x = i % 2 ? iw - 1 : 2, z = i < 2 ? 2 : id - 1;
                put(x, 1, z, B::GoldBlock);
                put(x + (i % 2 ? 1 : -1), 1, z, B::GoldBlock);
                put(x, 2, z, B::Candles);
            }
            put(cx + 2, 1, id, B::Chest);
            for (int z = 2; z < id; ++z)
                for (int x = cx - 1; x <= cx + 1; ++x)
                    if (!nearShaft(x, z)) bp.set(x, 0, z, B::RugRed);
            chest_quality = "3";
            break;
        case RoomKind::Boss: {
            // Canales de lava junto a los muros, anillo de columnas, trono y runas.
            for (int z = 1; z <= id; ++z) {
                bp.set(1, 0, z, B::Lava);
                bp.set(iw, 0, z, B::Lava);
            }
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * 0.785398f;
                const int x = cx + static_cast<int>(std::lround(std::cos(a) * (iw / 2 - 2)));
                const int z = cz + static_cast<int>(std::lround(std::sin(a) * (id / 2 - 2)));
                if (onPath(x, z)) continue;
                for (int y = 1; y <= ih; ++y) bp.set(x, y, z, y == 3 ? B::RuneStone : B::CarvedStone);
                bp.set(x, 1, z + (z < cz ? 1 : -1), B::Brazier);
            }
            for (int z = 2; z < id; ++z)
                for (int x = 2; x < iw; ++x) {
                    const float dd = std::hypot(static_cast<float>(x - cx), static_cast<float>(z - cz));
                    if (std::abs(dd - 3.0f) < 0.5f) bp.set(x, 0, z, B::RuneStone);
                }
            for (int x = cx - 2; x <= cx + 2; ++x) bp.set(x, 1, id, B::CarvedStone);
            bp.set(cx, 2, id, B::Throne);
            bp.set(cx - 2, 2, id, B::Brazier);
            bp.set(cx + 2, 2, id, B::Brazier);
            bp.set(cx + 1, 1, id - 1, B::Chest);
            bp.set(cx - 1, 1, id - 1, B::GoldBlock);
            for (int y = 2; y <= std::min(ih - 1, 5); ++y) {
                bp.set(cx - 3, y, D - 1, B::BannerRed);
                bp.set(cx + 3, y, D - 1, B::BannerRed);
            }
            ctx.point("jefe", static_cast<float>(cx), 1, static_cast<float>(cz + 2), 0, boss);
            ctx.point("cofre", static_cast<float>(cx + 1), 2, static_cast<float>(id - 1), 0, "4");
            break;
        }
        default:
            break;
    }
    if (!chest_quality.empty()) {
        const int x = kind == RoomKind::Library ? iw - 2 : cx + 2;
        const int z = kind == RoomKind::Library ? id - 1 : id;
        ctx.point("cofre", static_cast<float>(x), 2, static_cast<float>(z), 0, chest_quality);
    }
    if (kind != RoomKind::Boss && kind != RoomKind::Plain && !spawn.empty()) {
        ctx.point("spawner", static_cast<float>(cx + (shaft_here ? 3 : 0)), 1, static_cast<float>(cz + (shaft_here ? 3 : 0)), 0, spawn);
        if (level >= 4 && kind == RoomKind::Hall) ctx.point("spawner", static_cast<float>(cx - 2), 1, static_cast<float>(cz - 2), 0, spawn);
    }
}

void corridor(BuildContext& ctx, int length, bool trap) {
    ctx.bp = Blueprint(5, 5, length);
    Blueprint& bp = ctx.bp;
    for (int z = 0; z < length; ++z) {
        for (int x = 0; x < 5; ++x) {
            for (int y = 0; y < 5; ++y) {
                const bool shell = x == 0 || x == 4 || y == 0 || y == 4;
                BlockId m = B::Air;
                if (shell) {
                    const float r = ctx.rand(x, y, z);
                    m = r < 0.25f ? B::MossyBricks : (r < 0.33f ? B::CrackedBricks : B::DungeonBricks);
                    if (y == 0 && x > 0 && x < 4) m = B::StoneBricks;
                }
                bp.set(x, y, z, m);
            }
        }
        if (z % 7 == 3) bp.set(2, 3, z, B::Lantern);
        if (trap && z > 2 && z < length - 3 && z % 2 == 0) {
            for (int x = 1; x <= 3; ++x) bp.set(x, 1, z, B::Spikes);
        }
        if (ctx.rand(z, 8) < 0.15f) bp.set(ctx.rand(z, 9) < 0.5f ? 1 : 3, 3, z, B::Cobweb);
    }
    if (trap) ctx.point("trampa", 2, 1, static_cast<float>(length / 2), 0, "pinchos");
}

void spiralShaft(BuildContext& ctx, int depth, BlockId wall) {
    ctx.bp = Blueprint(5, depth + 4, 5);
    Blueprint& bp = ctx.bp;
    static constexpr int kRing[8][2] = {{1, 1}, {2, 1}, {3, 1}, {3, 2}, {3, 3}, {2, 3}, {1, 3}, {1, 2}};
    static constexpr int kOut[8][2] = {{1, 0}, {2, 0}, {3, 0}, {4, 2}, {4, 3}, {2, 4}, {1, 4}, {0, 2}};
    for (int y = 0; y <= depth + 1; ++y) {
        for (int z = 0; z < 5; ++z) {
            for (int x = 0; x < 5; ++x) {
                const bool ring_wall = x == 0 || z == 0 || x == 4 || z == 4;
                if (ring_wall) {
                    if (y <= depth + 1) bp.set(x, y, z, ctx.rand(x, y, z) < 0.25f ? B::MossyBricks : wall);
                } else if (x == 2 && z == 2) {
                    bp.set(x, y, z, y <= depth ? B::CarvedStone : B::Air);
                }
            }
        }
    }
    bp.set(2, depth + 1, 2, B::Lantern);
    for (int y = 1; y <= depth; ++y) {
        for (int i = 0; i < 8; ++i) {
            const bool step = ((depth - y) % 8) == i;
            bp.set(kRing[i][0], y, kRing[i][1], step ? B::StoneBricks : B::Air);
        }
    }
    for (int i = 0; i < 8; ++i) {
        bp.set(kRing[i][0], 0, kRing[i][1], B::StoneBricks);
        bp.set(kRing[i][0], depth + 1, kRing[i][1], B::Air);
        bp.set(kRing[i][0], depth + 2, kRing[i][1], B::Air);
    }
    // Entrada arriba (hueco en la barandilla junto al primer escalon).
    bp.set(kOut[0][0], depth + 1, kOut[0][1], B::Air);
    // Salida abajo, junto a la celda del anillo que queda al nivel del suelo.
    const int e = depth % 8;
    bp.set(kOut[e][0], 1, kOut[e][1], B::Air);
    bp.set(kOut[e][0], 2, kOut[e][1], B::Air);
    // Luz en el pozo (setas luminosas en la columna cada pocos metros).
    for (int y = 4; y < depth; y += 6) bp.set(2, y, 2, B::Glowstone);
}

void dungeonEntrance(BuildContext& ctx, int shaft_depth) {
    const int S = 11;
    ctx.bp = Blueprint(S, 8, S);
    Blueprint& bp = ctx.bp;
    const int c = S / 2;
    const auto stone = [&](int x, int y, int z) {
        const float r = ctx.rand(x, y, z);
        return r < 0.4f ? B::MossyBricks : (r < 0.55f ? B::CrackedBricks : B::StoneBricks);
    };
    for (int z = 0; z < S; ++z) {
        for (int x = 0; x < S; ++x) {
            if (std::abs(x - c) <= 2 && std::abs(z - c) <= 2) continue;  // el pozo
            bp.set(x, 0, z, (x == 0 || z == 0 || x == S - 1 || z == S - 1) ? stone(x, 0, z) : B::CarvedStone);
            for (int y = 1; y < 8; ++y) bp.set(x, y, z, B::Air);
        }
    }
    // Columnas rotas en las esquinas y un dintel sobre la entrada.
    const int pillars[4][2] = {{1, 1}, {S - 2, 1}, {1, S - 2}, {S - 2, S - 2}};
    for (int i = 0; i < 4; ++i) {
        const int h = 3 + static_cast<int>(ctx.rand(i, 3) * 4.0f);
        for (int y = 1; y <= h; ++y) bp.set(pillars[i][0], y, pillars[i][1], y == h ? B::CarvedStone : stone(pillars[i][0], y, pillars[i][1]));
    }
    for (int x = 1; x <= S - 2; ++x) {
        if (ctx.rand(x, 11) < 0.75f) bp.set(x, 6, 1, stone(x, 6, 1));
    }
    bp.set(c - 3, 1, 1, B::Brazier);
    bp.set(c + 3, 1, 1, B::Brazier);
    bp.set(c - 3, 1, c - 3, B::Bones);
    bp.set(c + 3, 1, c + 2, B::Cobweb);
    ctx.point("entrada", static_cast<float>(c - 1), 1, static_cast<float>(c - 3), 2, std::to_string(shaft_depth));
}

}  // namespace build

}  // namespace cramion::voxel
