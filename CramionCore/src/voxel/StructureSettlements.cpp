// Asentamientos nuevos (Structures.h): farolas, arbol mayor de las aldeas
// elficas, casas sobre pilotes del pantano, estanques, y las plazas HOSTILES
// (fuerte de bandidos, campamento de guerra orco, castillo no muerto, ciudad
// maldita y torre del culto). Cada plaza hostil es un solo plano grande con la
// entrada hacia -z; deja puntos "spawner" (guarnicion), "jefe" y "cofre" que
// el juego puebla (Assets/Scripts/Mazmorras.cpp). Todo determinista (ctx.seed).

#include "Structures.h"

#include <algorithm>
#include <cmath>

namespace cramion::voxel {

namespace B = block;

namespace build {

namespace {

constexpr float kPi = 3.14159265f;

// Bloque de luz del agente de luces (ids 181..190) o el de repuesto si aun no
// esta registrado en el catalogo (nombre vacio).
BlockId lightOr(BlockId wanted, BlockId fallback) { return blockDef(wanted).name.empty() ? fallback : wanted; }
BlockId torchBlock() { return lightOr(B::StandingTorch, B::Torch); }
BlockId campfireBlock() { return lightOr(B::Campfire, B::Fire); }
BlockId chandelierBlock() { return lightOr(B::Chandelier, B::Lantern); }

// Aire en las celdas sin tocar de una caja (quita la hierba y las plantas del terreno).
void clearBox(Blueprint& bp, int x0, int y0, int z0, int x1, int y1, int z1) {
    for (int y = y0; y <= y1; ++y)
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                if (bp.inside(x, y, z) && bp.get(x, y, z) == kKeep) bp.set(x, y, z, B::Air);
}

// Pone si la celda esta libre (aire o sin tocar).
bool put(Blueprint& bp, int x, int y, int z, BlockId id) {
    if (!bp.inside(x, y, z)) return false;
    const BlockId c = bp.get(x, y, z);
    if (c != kKeep && c != B::Air) return false;
    bp.set(x, y, z, id);
    return true;
}

// Escalera recta: n escalones desde el suelo y0 avanzando (dx, dz); abre 3 de aire encima.
void stairs(Blueprint& bp, int x, int y0, int z, int n, int dx, int dz, BlockId step) {
    for (int j = 0; j < n; ++j) {
        const int sx = x + dx * j, sz = z + dz * j;
        bp.set(sx, y0 + 1 + j, sz, step);
        for (int h = 1; h <= 3; ++h) bp.set(sx, y0 + 1 + j + h, sz, B::Air);
    }
}

// Arbol muerto: tronco y ramas secas sin hojas.
void deadTree(BuildContext& ctx, int x, int y0, int z, int salt) {
    Blueprint& bp = ctx.bp;
    const int h = 4 + static_cast<int>(ctx.rand(x, z, salt) * 4.0f);
    const BlockId wood = ctx.rand(z, x, salt + 1) < 0.5f ? B::SpruceLog : B::OakLog;
    for (int y = 1; y <= h; ++y) bp.set(x, y0 + y, z, wood);
    static constexpr int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int i = 0; i < 4; ++i) {
        if (ctx.rand(x + i, z, salt + 2) < 0.4f) continue;
        const int by = y0 + h - 1 - (i % 2);
        bp.set(x + kDirs[i][0], by, z + kDirs[i][1], wood);
        if (ctx.rand(x, z + i, salt + 3) < 0.6f) bp.set(x + kDirs[i][0] * 2, by + 1, z + kDirs[i][1] * 2, wood);
    }
}

// Lapida (con huesos o una calavera delante a veces).
void grave(BuildContext& ctx, int x, int y0, int z, int salt) {
    Blueprint& bp = ctx.bp;
    bp.set(x, y0 + 1, z, B::Gravestone);
    const float r = ctx.rand(x, z, salt);
    if (r < 0.25f) put(bp, x, y0 + 1, z - 1, B::Bones);
    else if (r < 0.35f) put(bp, x, y0 + 1, z - 1, B::SkullPile);
    else if (r < 0.45f) put(bp, x, y0 + 1, z - 1, B::Candles);
    if (bp.get(x, y0, z - 1) == kKeep || bp.get(x, y0, z - 1) == B::Air) bp.set(x, y0, z - 1, B::DirtPath);  // tierra removida
}

// Tienda de campana a dos aguas (5 de ancho, abierta por delante: -z).
void aTent(BuildContext& ctx, int x0, int y0, int z0, int len, BlockId cloth) {
    Blueprint& bp = ctx.bp;
    for (int z = z0; z < z0 + len; ++z) {
        for (int x = x0; x <= x0 + 4; ++x)
            for (int y = y0 + 1; y <= y0 + 3; ++y) bp.set(x, y, z, B::Air);
        bp.set(x0, y0 + 1, z, cloth);
        bp.set(x0 + 4, y0 + 1, z, cloth);
        bp.set(x0 + 1, y0 + 2, z, cloth);
        bp.set(x0 + 3, y0 + 2, z, cloth);
        bp.set(x0 + 2, y0 + 3, z, cloth);
        for (int x = x0 + 1; x <= x0 + 3; ++x) bp.set(x, y0, z, z == z0 ? B::DirtPath : B::RugRed);
    }
    // Fondo cerrado y palos en la entrada.
    const int zb = z0 + len - 1;
    for (int x = x0 + 1; x <= x0 + 3; ++x) bp.set(x, y0 + 1, zb, cloth);
    bp.set(x0 + 2, y0 + 2, zb, cloth);
    bp.set(x0 + 2, y0 + 1, z0, B::Air);
    bp.set(x0 + 1, y0 + 1, zb - 1, B::Barrel);
}

// Tienda conica (pieles) alrededor de un palo: radio r, alto h; la puerta mira a (fx, fz).
void coneTent(BuildContext& ctx, int cx, int y0, int cz, int r, int h, BlockId cloth, float fx, float fz) {
    Blueprint& bp = ctx.bp;
    const float fl = std::max(0.001f, std::hypot(fx, fz));
    fx /= fl;
    fz /= fl;
    for (int y = 1; y <= h; ++y) {
        const float rr = static_cast<float>(r) * (1.0f - static_cast<float>(y - 1) / static_cast<float>(h)) + 0.35f;
        for (int z = cz - r - 1; z <= cz + r + 1; ++z)
            for (int x = cx - r - 1; x <= cx + r + 1; ++x) {
                const float dx = static_cast<float>(x - cx), dz = static_cast<float>(z - cz);
                const float d = std::hypot(dx, dz);
                if (d > rr) continue;
                const bool shell = d > rr - 1.0f;
                const bool door = shell && y <= 2 && d > 0.5f && (dx * fx + dz * fz) / d > 0.8f;
                bp.set(x, y0 + y, z, shell && !door ? cloth : B::Air);
                if (y == 1) bp.set(x, y0, z, d < rr - 1.0f ? B::RugRed : B::DirtPath);
            }
    }
    for (int y = 1; y <= h + 1; ++y) bp.set(cx, y0 + y, cz, B::SpruceLog);
}

// Piedra de muralla en ruinas (ladrillo agrietado, musgoso, roca, ladrillo).
BlockId ruinStone(const BuildContext& ctx, int x, int y, int z) {
    const float r = ctx.rand(x, y, z);
    return r < 0.3f ? B::MossyBricks : (r < 0.5f ? B::CrackedBricks : (r < 0.68f ? B::Cobblestone : B::StoneBricks));
}

// Piedra negra (castillo no muerto, torre del culto).
BlockId darkStone(const BuildContext& ctx, int x, int y, int z) {
    const float r = ctx.rand(x, y, z + 101);
    return r < 0.16f ? B::CrackedBlackstone : (r < 0.24f ? B::MossyBricks : B::BlackstoneBricks);
}

const std::string& pickTag(const std::vector<std::string>& tags, std::size_t i) {
    static const std::string kNone;
    return tags.empty() ? kNone : tags[i % tags.size()];
}

}  // namespace

// --- Farola de calle y estanque ------------------------------------------------------------

void streetLamp(BuildContext& ctx) {
    // Mismo tamano que lampPost (1 x 5 x 1): poste de hierro y cabeza de farola
    // del agente de luces; sin ellas, el poste de madera con farol de siempre.
    ctx.bp = Blueprint(1, 5, 1);
    const bool lights = !blockDef(B::StreetLamp).name.empty() && !blockDef(B::LampPost).name.empty();
    ctx.bp.set(0, 0, 0, B::Cobblestone);
    for (int y = 1; y <= 3; ++y) ctx.bp.set(0, y, 0, lights ? B::LampPost : B::Timber);
    ctx.bp.set(0, 4, 0, lights ? B::StreetLamp : B::Lantern);
}

void wallTorches(BuildContext& ctx, int every) {
    // Plano de wallSegment: alto + 2; el adarve (losas) en y = alto - 1 y z 1..3.
    Blueprint& bp = ctx.bp;
    const int height = bp.h - 2;
    if (height < 2 || every < 1) return;
    for (int x = every / 2; x < bp.w; x += every)
        if (bp.get(x, height - 1, 3) == B::Paving && bp.get(x, height, 3) == kKeep) bp.set(x, height, 3, torchBlock());
}

// CIUDADES-AMPLIAS: jardin entre casas (w x d, la entrada mira a -z): seto con
// entrada y faroles, camino de losas hasta los bancos, arbol en medio y flores.
void garden(BuildContext& ctx, int w, int d) {
    w = std::max(w, 7);
    d = std::max(d, 7);
    BuildContext tc;
    tc.seed = ctx.seed ^ 0x9A4D3Eu;
    tc.style = ctx.style;
    tree(tc);
    ctx.bp = Blueprint(w, std::max(4, tc.bp.h), d);
    Blueprint& bp = ctx.bp;
    const int cx = w / 2, cz = d / 2 + 1;
    static const BlockId kFlowers[] = {B::RedFlower, B::YellowFlower, B::BlueFlower, B::WhiteFlower, B::Lavender};
    for (int z = 0; z < d; ++z) {
        for (int x = 0; x < w; ++x) {
            const bool edge = x == 0 || z == 0 || x == w - 1 || z == d - 1;
            const bool gate = z == 0 && std::abs(x - cx) <= 1;
            const bool path = (x == cx && z < cz - 1) || (z == cz - 2 && std::abs(x - cx) <= 2);
            bp.set(x, 0, z, path || gate ? B::Paving : B::Grass);
            if (edge && !gate) {
                bp.set(x, 1, z, B::Bush);
            } else if (!path && !gate && ctx.rand(x, z, 31) < 0.22f) {
                bp.set(x, 1, z, kFlowers[static_cast<int>(ctx.rand(z, x, 32) * 5.0f) % 5]);
            } else {
                bp.set(x, 1, z, B::Air);
            }
        }
    }
    // Faroles en los postes de la entrada.
    for (int x : {cx - 2, cx + 2}) {
        if (x <= 0 || x >= w - 1) continue;
        bp.set(x, 1, 0, B::Fence);
        bp.set(x, 2, 0, B::Lantern);
    }
    // Arbol (el plano de tree, 7 x 7, centrado en (cx, cz)).
    for (int y = 0; y < tc.bp.h; ++y)
        for (int z = 0; z < tc.bp.d; ++z)
            for (int x = 0; x < tc.bp.w; ++x) {
                const BlockId id = tc.bp.get(x, y, z);
                const int gx = cx - 3 + x, gz = cz - 3 + z;
                if (id == kKeep || !bp.inside(gx, y, gz)) continue;
                bp.set(gx, y, gz, id);
            }
    // Bancos delante del arbol, a los lados del camino.
    if (cx - 2 > 0 && cz - 3 > 0) bp.set(cx - 2, 1, cz - 3, B::Bench);
    if (cx + 2 < w - 1 && cz - 3 > 0) bp.set(cx + 2, 1, cz - 3, B::Bench);
}

void pond(BuildContext& ctx, int w, int d) {
    ctx.bp = Blueprint(w, 3, d);
    Blueprint& bp = ctx.bp;
    const float ax = static_cast<float>(w) * 0.5f, az = static_cast<float>(d) * 0.5f;
    for (int z = 0; z < d; ++z)
        for (int x = 0; x < w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f - ax) / ax, v = (static_cast<float>(z) + 0.5f - az) / az;
            const float e = u * u + v * v + (ctx.rand(x, z) - 0.5f) * 0.25f;
            if (e < 0.6f) {
                bp.set(x, 0, z, B::PoolWater);
                bp.set(x, 1, z, B::Air);
                bp.set(x, 2, z, B::Air);
            } else if (e < 1.0f) {
                bp.set(x, 0, z, ctx.rand(z, x) < 0.5f ? B::Moss : B::Clay);
                const float r = ctx.rand(x, z, 7);
                bp.set(x, 1, z, r < 0.45f ? B::Reeds : (r < 0.55f ? B::Fern : B::Air));
            }
        }
}

// --- Aldea elfica: el arbol mayor de la plaza --------------------------------------------

void greatTree(BuildContext& ctx) {
    constexpr int S = 15, H = 22;
    ctx.bp = Blueprint(S, H, S);
    Blueprint& bp = ctx.bp;
    const int c = S / 2;
    // Corro de piedra con musgo y bancos alrededor.
    for (int z = 0; z < S; ++z)
        for (int x = 0; x < S; ++x) {
            const float d = std::hypot(static_cast<float>(x - c), static_cast<float>(z - c));
            if (d > c + 0.3f) continue;
            if (d > c - 1.2f) bp.set(x, 0, z, B::MossyBricks);
            else if (d > 2.5f) bp.set(x, 0, z, B::Moss);
            for (int y = 1; y < 5; ++y) bp.set(x, y, z, B::Air);
        }
    // Tronco de 2 x 2 con raices.
    const int trunk = 11;
    for (int y = 0; y <= trunk; ++y)
        for (int dz = 0; dz <= 1; ++dz)
            for (int dx = 0; dx <= 1; ++dx) bp.set(c - 1 + dx, y, c - 1 + dz, B::OakLog);
    static constexpr int kRoots[4][2] = {{-2, 0}, {1, -2}, {2, 1}, {-1, 2}};
    for (const auto& rt : kRoots) {
        bp.set(c + rt[0], 1, c + rt[1], B::OakLog);
        bp.set(c + rt[0], 0, c + rt[1], B::OakLog);
    }
    // Ramas y copa ancha (dos capas de hojas).
    static constexpr int kBranch[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    // (el tronco ocupa c-1..c: las ramas salen de su borde)
    for (int i = 0; i < 4; ++i)
        for (int k = 1; k <= 3; ++k) {
            const int bx = kBranch[i][0] > 0 ? c + k : (kBranch[i][0] < 0 ? c - 1 - k : c - (i % 2));
            const int bz = kBranch[i][1] > 0 ? c + k : (kBranch[i][1] < 0 ? c - 1 - k : c - (i % 2));
            bp.set(bx, trunk - 2 + k / 2, bz, B::OakLog);
        }
    for (int y = trunk - 2; y < H - 1; ++y)
        for (int z = 0; z < S; ++z)
            for (int x = 0; x < S; ++x) {
                const float dy = static_cast<float>(y - (trunk + 3)) * 1.6f;
                const float dx = static_cast<float>(x) - static_cast<float>(c) + 0.5f, dz = static_cast<float>(z) - static_cast<float>(c) + 0.5f;
                const float d2 = dx * dx + dz * dz + dy * dy;
                const float R = 6.6f;
                if (d2 > R * R) continue;
                if (d2 > (R - 1.3f) * (R - 1.3f) && ctx.rand(x, y, z) < 0.3f) continue;
                if (bp.get(x, y, z) == B::OakLog) continue;
                bp.set(x, y, z, ctx.rand(z, y, x) < 0.25f ? B::BirchLeaves : B::OakLeaves);
            }
    // Farolillos colgando de la copa y bancos alrededor del tronco.
    const BlockId lamp = lightOr(B::PaperLantern, B::Lantern);
    for (int i = 0; i < 6; ++i) {
        const float a = static_cast<float>(i) * kPi / 3.0f + 0.4f;
        const int x = c + static_cast<int>(std::lround(std::cos(a) * 4.5f)), z = c + static_cast<int>(std::lround(std::sin(a) * 4.5f));
        // Cuelga del primer bloque de hojas por encima.
        for (int y = trunk - 3; y < H - 1; ++y) {
            if (bp.get(x, y + 1, z) == B::OakLeaves || bp.get(x, y + 1, z) == B::BirchLeaves) {
                if (y >= 4) bp.set(x, y, z, lamp);
                break;
            }
        }
    }
    for (int i = 0; i < 4; ++i) {  // bancos en las diagonales
        const int bx = c + (i % 2 ? 3 : -4), bz = c + (i < 2 ? 3 : -4);
        bp.set(bx, 1, bz, B::Bench);
        bp.set(bx + 1, 1, bz, B::Bench);
    }
    for (int i = 0; i < 4; ++i) {
        const int fx = c + (i % 2 ? 5 : -5), fz = c + (i < 2 ? 5 : -5);
        put(bp, fx, 1, fz, i % 2 ? B::BlueFlower : B::WhiteFlower);
    }
    // Estandartes elficos colgados del tronco.
    for (int y = 3; y <= 5; ++y) {
        bp.set(c - 2, y, c, B::BannerGreen);
        bp.set(c + 1, y, c - 1, B::BannerGreen);
    }
    ctx.point("plaza", static_cast<float>(c), 1, static_cast<float>(c - 5), 2);
}

// --- Casa sobre pilotes (pantano) -----------------------------------------------------------

void raise(BuildContext& ctx, int dy) {
    if (dy <= 0) return;
    const Blueprint old = std::move(ctx.bp);
    Blueprint bp(old.w, old.h + dy, old.d + dy);
    // Copia desplazada (dy arriba y dy hacia el fondo: delante queda sitio para la escalera).
    for (int y = 0; y < old.h; ++y)
        for (int z = 0; z < old.d; ++z)
            for (int x = 0; x < old.w; ++x) bp.set(x, y + dy, z + dy, old.get(x, y, z));
    // Pilotes bajo el cimiento (esquinas y cada 3) y la escalera de la puerta.
    int door_x = -1;
    for (int z = 0; z < old.d && door_x < 0; ++z)
        for (int x = 0; x < old.w; ++x)
            if (old.get(x, 1, z) == B::Door) {
                door_x = x;
                break;
            }
    for (int z = 0; z < old.d; ++z)
        for (int x = 0; x < old.w; ++x) {
            const BlockId f = old.get(x, 0, z);
            if (f == kKeep || f == B::Air) continue;
            const bool edge_x = x == 0 || old.get(x - 1, 0, z) == kKeep || x == old.w - 1 || old.get(x + 1, 0, z) == kKeep;
            const bool edge_z = z == 0 || old.get(x, 0, z - 1) == kKeep || z == old.d - 1 || old.get(x, 0, z + 1) == kKeep;
            if ((edge_x && edge_z) || ((edge_x || edge_z) && (x + z) % 3 == 0)) {
                for (int y = 0; y < dy; ++y) bp.set(x, y, z + dy, B::OakLog);
            }
        }
    if (door_x >= 0) {
        for (int k = 1; k <= dy; ++k) {
            const int z = dy - k, y = dy - k;  // escalon k: baja hacia delante
            bp.set(door_x, y, z, B::DarkPlanks);
            for (int h = 1; h <= 3; ++h)
                if (bp.get(door_x, y + h, z) == kKeep) bp.set(door_x, y + h, z, B::Air);
            if (k < dy) {
                bp.set(door_x - 1, y + 1, z, B::Fence);
                bp.set(door_x + 1, y + 1, z, B::Fence);
            }
        }
    }
    ctx.bp = std::move(bp);
    for (LocalPoint& p : ctx.points) {
        p.y += static_cast<float>(dy);
        p.z += static_cast<float>(dy);
    }
}

// --- Fuerte de bandidos ------------------------------------------------------------------

void banditFort(BuildContext& ctx, int S, const std::string& grunt, const std::string& archer, const std::string& boss) {
    S |= 1;
    ctx.bp = Blueprint(S, 17, S);
    Blueprint& bp = ctx.bp;
    const int c = S / 2;
    clearBox(bp, 1, 1, 1, S - 2, 12, S - 2);
    // Patio de tierra pisada y grava.
    for (int z = 1; z < S - 1; ++z)
        for (int x = 1; x < S - 1; ++x) {
            const float r = ctx.rand(x, 0, z);
            if (r < 0.45f) bp.set(x, 0, z, B::DirtPath);
            else if (r < 0.6f) bp.set(x, 0, z, B::Gravel);
        }
    for (int z = 0; z < S; ++z) bp.set(c, 0, z, B::DirtPath);
    // Empalizada de troncos afilados (7 de alto) con puntas alternas.
    for (int z = 0; z < S; ++z)
        for (int x = 0; x < S; ++x) {
            if (x != 0 && z != 0 && x != S - 1 && z != S - 1) continue;
            for (int y = 0; y <= 6; ++y) bp.set(x, y, z, B::Palisade);
            if ((x + z) % 2 == 0) bp.set(x, 7, z, B::Fence);
        }
    // Puerta (5 de ancho) con dintel y estandartes de calavera.
    for (int x = c - 2; x <= c + 2; ++x) {
        bp.set(x, 0, 0, B::DirtPath);
        for (int y = 1; y <= 5; ++y) bp.set(x, y, 0, B::Air);
        bp.set(x, 7, 0, B::Air);
    }
    for (int y = 2; y <= 5; ++y) {
        bp.set(c - 3, y, 0, B::BannerSkull);
        bp.set(c + 3, y, 0, B::BannerSkull);
    }
    bp.set(c - 3, 7, 0, B::SkullPile);
    bp.set(c + 3, 7, 0, B::SkullPile);
    // Adarve de tablones por dentro (suelo a 4) con postes.
    for (int z = 1; z < S - 1; ++z)
        for (int x = 1; x < S - 1; ++x) {
            if (x != 1 && z != 1 && x != S - 2 && z != S - 2) continue;
            bp.set(x, 4, z, B::OakPlanks);
            const bool gate = z == 1 && std::abs(x - c) <= 2;
            if (!gate && (x + z) % 4 == 0)
                for (int y = 1; y <= 3; ++y) bp.set(x, y, z, B::Fence);
            if (!gate && (x * 3 + z) % 9 == 0) bp.set(x, 5, z, torchBlock());
        }
    // Torres de vigia en las esquinas (5 x 5): plataforma a la altura del adarve,
    // atalaya arriba con barandilla y tejado de paja.
    const auto watchtower = [&](int ox, int oz) {
        for (int y = 0; y <= 11; ++y) {
            bp.set(ox, y, oz, B::SpruceLog);
            bp.set(ox + 4, y, oz, B::SpruceLog);
            bp.set(ox, y, oz + 4, B::SpruceLog);
            bp.set(ox + 4, y, oz + 4, B::SpruceLog);
        }
        for (int z = oz; z <= oz + 4; ++z)
            for (int x = ox; x <= ox + 4; ++x) {
                const bool corner = (x == ox || x == ox + 4) && (z == oz || z == oz + 4);
                if (corner) continue;
                bp.set(x, 4, z, B::OakPlanks);
                bp.set(x, 8, z, B::OakPlanks);
                for (int y = 5; y <= 7; ++y) bp.set(x, y, z, B::Air);
                const bool rim = x == ox || x == ox + 4 || z == oz || z == oz + 4;
                bp.set(x, 9, z, rim ? B::Fence : B::Air);
                bp.set(x, 10, z, B::Air);
                bp.set(x, 11, z, B::Air);
            }
        for (int k = 0; k <= 3; ++k)
            for (int z = oz - 1 + k; z <= oz + 5 - k; ++z)
                for (int x = ox - 1 + k; x <= ox + 5 - k; ++x) bp.set(x, 12 + k, z, B::Thatch);
        stairs(bp, ox + 1, 4, oz + 2, 3, 1, 0, B::OakPlanks);
        bp.set(ox + 2, 9, oz + 2, B::Lantern);
    };
    watchtower(1, 1);
    watchtower(S - 6, 1);
    watchtower(1, S - 6);
    watchtower(S - 6, S - 6);
    // Casa larga del jefe al fondo.
    const int lx0 = c - 7, lx1 = c + 7, lz1 = S - 4, lz0 = lz1 - 11;
    for (int z = lz0; z <= lz1; ++z)
        for (int x = lx0; x <= lx1; ++x) {
            const bool wall = x == lx0 || x == lx1 || z == lz0 || z == lz1;
            const bool corner = (x == lx0 || x == lx1) && (z == lz0 || z == lz1);
            bp.set(x, 0, z, wall ? B::Cobblestone : B::OakPlanks);
            for (int y = 1; y <= 4; ++y) {
                BlockId m = B::Air;
                if (wall) {
                    const int along = (x == lx0 || x == lx1) ? z - lz0 : x - lx0;
                    m = corner || along % 4 == 0 ? B::SpruceLog : B::DarkPlanks;
                    if (!corner && along % 4 == 2 && y == 2) m = B::Fence;
                }
                bp.set(x, y, z, m);
            }
        }
    for (int k = 0;; ++k) {
        const int za = lz0 - 1 + k, zb = lz1 + 1 - k;
        if (za > zb) break;
        for (int x = lx0 - 1; x <= lx1 + 1; ++x) {
            bp.set(x, 5 + k, za, B::Thatch);
            bp.set(x, 5 + k, zb, B::Thatch);
        }
        if (k == 0) continue;
        for (int z = za + 1; z < zb; ++z)
            for (int x = lx0; x <= lx1; ++x) bp.set(x, 5 + k, z, (x == lx0 || x == lx1) ? B::DarkPlanks : B::Air);
    }
    for (int y = 1; y <= 2; ++y) {
        bp.set(c, y, lz0, B::Door);
        bp.set(c + 1, y, lz0, B::Air);
        bp.set(c - 1, y, lz0, B::Air);
    }
    bp.set(c - 2, 3, lz0 - 1, B::Lantern);
    bp.set(c + 2, 3, lz0 - 1, B::Lantern);
    // Dentro: mesa del botin, asiento del jefe, camastros, armas y cofres.
    bp.set(c, 1, lz1 - 1, B::Throne);
    bp.set(c - 1, 1, lz1 - 1, B::GoldBlock);
    bp.set(c + 1, 1, lz1 - 1, B::Crate);
    for (int x = c - 2; x <= c + 2; ++x) bp.set(x, 1, lz0 + 5, B::Table);
    bp.set(c - 1, 2, lz0 + 5, B::Candles);
    bp.set(c + 1, 2, lz0 + 5, B::GoldBlock);
    for (int x = c - 2; x <= c + 2; x += 2) {
        bp.set(x, 1, lz0 + 4, B::Chair);
        bp.set(x, 1, lz0 + 6, B::Chair);
    }
    for (int z = lz0 + 2; z < lz1 - 1; z += 3) {
        bp.set(lx0 + 1, 1, z, B::Bed);
        bp.set(lx0 + 2, 1, z, B::Bed);
        bp.set(lx1 - 1, 1, z, B::WeaponRack);
    }
    bp.set(lx1 - 1, 1, lz1 - 1, B::Barrel);
    bp.set(lx1 - 2, 1, lz1 - 1, B::Barrel);
    bp.set(lx1 - 1, 2, lz1 - 1, B::Crate);
    for (int z = lz0 + 2; z < lz1; z += 4) bp.set(c, 4, z, B::Lantern);
    ctx.point("jefe", static_cast<float>(c), 1, static_cast<float>(lz1 - 3), 0, boss);
    ctx.point("cofre", static_cast<float>(c + 3), 1, static_cast<float>(lz1 - 1), 0, "3");
    ctx.point("spawner", static_cast<float>(c - 3), 1, static_cast<float>(lz0 + 2), 0, grunt);
    // Tiendas en el patio, con botin.
    const int tz0 = c - 7, tz1 = c + 1;
    const int txl = 8, txr = S - 13;
    aTent(ctx, txl, 0, tz0, 5, B::ClothRed);
    aTent(ctx, txr, 0, tz0, 5, B::HideTent);
    aTent(ctx, txl, 0, tz1, 5, B::HideTent);
    aTent(ctx, txr, 0, tz1, 5, B::ClothRed);
    ctx.point("cofre", static_cast<float>(txl + 3), 1, static_cast<float>(tz0 + 3), 0, "1");
    ctx.point("cofre", static_cast<float>(txr + 1), 1, static_cast<float>(tz1 + 3), 0, ctx.rand(5) < 0.4f ? "2" : "1");
    // Hoguera con asientos de tronco.
    const int fz = c - 1;
    for (int z = fz - 1; z <= fz + 1; ++z)
        for (int x = c - 1; x <= c + 1; ++x) bp.set(x, 0, z, B::Cobblestone);
    bp.set(c, 1, fz, campfireBlock());
    bp.set(c - 3, 1, fz, B::OakLog);
    bp.set(c + 3, 1, fz, B::OakLog);
    bp.set(c, 1, fz + 3, B::OakLog);
    // Munecos de practica, jaula y montones de lo robado.
    for (int i = 0; i < 3; ++i) {
        const int x = c - 6 + i * 6, z = 6;
        if (std::abs(x - c) <= 2) continue;
        bp.set(x, 1, z, B::Fence);
        bp.set(x, 2, z, B::HayBale);
    }
    for (int z = c + 6; z <= c + 8; ++z)
        for (int x = 3; x <= 5; ++x) {
            const bool ring = x == 3 || x == 5 || z == c + 6 || z == c + 8;
            for (int y = 1; y <= 2; ++y) bp.set(x, y, z, ring ? B::IronBars : B::Air);
            bp.set(x, 3, z, B::OakPlanks);
        }
    bp.set(4, 1, c + 7, B::Bones);
    for (int z = 7; z < S - 7; z += 2) {
        if (ctx.rand(z, 3) < 0.45f) put(bp, S - 3, 1, z, ctx.rand(z, 4) < 0.5f ? B::Crate : B::Barrel);
        if (ctx.rand(z, 5) < 0.3f) put(bp, S - 3, 2, z, B::Crate);
    }
    bp.set(c - 3, 1, 2, torchBlock());
    bp.set(c + 3, 1, 2, torchBlock());
    // Guarnicion: bandidos en el patio y en la puerta, arqueros en las torres.
    ctx.point("spawner", static_cast<float>(c - 5), 1, static_cast<float>(c - 1), 0, grunt);
    ctx.point("spawner", static_cast<float>(c + 5), 1, static_cast<float>(c - 1), 0, grunt);
    ctx.point("spawner", static_cast<float>(c), 1, 5, 0, grunt);
    ctx.point("spawner", static_cast<float>(c), 1, static_cast<float>(c + 4), 0, grunt);
    ctx.point("spawner", 3, 9, 3, 0, archer);
    ctx.point("spawner", static_cast<float>(S - 4), 9, 3, 0, archer);
    ctx.point("spawner", 3, 5, static_cast<float>(S - 4), 0, archer);
    ctx.point("spawner", static_cast<float>(S - 4), 5, static_cast<float>(S - 4), 0, archer);
}

// --- Campamento de guerra orco -------------------------------------------------------------

void orcCamp(BuildContext& ctx, int R, const std::string& grunt, const std::string& archer, const std::string& boss) {
    const int S = 2 * R + 9;
    ctx.bp = Blueprint(S, 16, S);
    Blueprint& bp = ctx.bp;
    const int c = S / 2;
    const auto dist = [&](int x, int z) { return std::hypot(static_cast<float>(x - c), static_cast<float>(z - c)); };
    for (int z = 0; z < S; ++z)
        for (int x = 0; x < S; ++x) {
            const float d = dist(x, z);
            if (d < static_cast<float>(R) - 0.5f) {
                for (int y = 1; y <= 12; ++y) bp.set(x, y, z, B::Air);
                const float r = ctx.rand(x, 0, z);
                if (r < 0.4f) bp.set(x, 0, z, B::DirtPath);
                else if (r < 0.55f) bp.set(x, 0, z, B::Gravel);
                else if (r < 0.6f) bp.set(x, 0, z, B::Cobblestone);
            } else if (d < static_cast<float>(R) + 0.6f) {
                // Empalizada en circulo.
                for (int y = 0; y <= 5; ++y) bp.set(x, y, z, B::Palisade);
                if (ctx.rand(x, 6, z) < 0.6f) bp.set(x, 6, z, B::Fence);
            } else if (d > static_cast<float>(R) + 1.5f && d < static_cast<float>(R) + 2.6f && (x * 7 + z * 3) % 3 == 0) {
                // Estacas hacia fuera.
                bp.set(x, 1, z, B::Fence);
                if (ctx.rand(x, 1, z) < 0.5f) bp.set(x, 2, z, B::Fence);
            }
        }
    // Puerta delante (-z) con dos torres de troncos coronadas de calaveras.
    const int gz = c - R;
    for (int z = gz - 2; z <= gz + 2; ++z)
        for (int x = c - 2; x <= c + 2; ++x) {
            if (dist(x, z) < static_cast<float>(R) - 1.5f) continue;
            bp.set(x, 0, z, B::DirtPath);
            for (int y = 1; y <= 7; ++y) bp.set(x, y, z, B::Air);
        }
    for (int side = -1; side <= 1; side += 2) {
        const int tx = c + side * 4;
        for (int z = gz - 1; z <= gz + 1; ++z)
            for (int x = tx - 1; x <= tx + 1; ++x)
                for (int y = 0; y <= 8; ++y) bp.set(x, y, z, B::SpruceLog);
        for (int x = tx - 1; x <= tx + 1; ++x) bp.set(x, 9, gz - 1, B::Palisade);
        bp.set(tx, 9, gz, B::Brazier);
        bp.set(tx - 1, 9, gz + 1, B::SkullPile);
        bp.set(tx + 1, 9, gz + 1, B::SkullPile);
        for (int y = 4; y <= 7; ++y) bp.set(tx, y, gz - 2, B::BannerSkull);
    }
    // Hoguera grande en el centro (brasas de magma) y asientos.
    for (int z = c - 1; z <= c + 1; ++z)
        for (int x = c - 1; x <= c + 1; ++x) bp.set(x, 0, z, B::Magma);
    bp.set(c, 1, c, campfireBlock());
    for (int i = 0; i < 6; ++i) {
        const float a = static_cast<float>(i) * kPi / 3.0f;
        put(bp, c + static_cast<int>(std::lround(std::cos(a) * 3.2f)), 1, c + static_cast<int>(std::lround(std::sin(a) * 3.2f)), B::OakLog);
    }
    // Totem de guerra detras de la hoguera.
    const int tz = c + 5;
    for (int y = 1; y <= 7; ++y) bp.set(c, y, tz, B::SpruceLog);
    bp.set(c, 8, tz, B::SkullPile);
    for (int y = 4; y <= 6; ++y) {
        bp.set(c - 1, y, tz, B::BannerSkull);
        bp.set(c + 1, y, tz, B::BannerSkull);
    }
    put(bp, c - 1, 1, tz, B::Bones);
    put(bp, c + 1, 1, tz, B::SkullPile);
    put(bp, c - 1, 1, tz + 1, B::Barrel);  // tambores de guerra
    put(bp, c + 1, 1, tz + 1, B::Barrel);
    // Gran tienda del caudillo al fondo.
    const int bz = c + R - 7;
    coneTent(ctx, c, 0, bz, 5, 8, B::HideTent, 0.0f, -1.0f);
    bp.set(c, 1, bz + 3, B::Throne);
    bp.set(c - 1, 1, bz + 3, B::SkullPile);
    bp.set(c + 1, 1, bz + 3, B::SkullPile);
    bp.set(c - 2, 1, bz + 2, B::Brazier);
    bp.set(c + 2, 1, bz + 2, B::Brazier);
    bp.set(c + 3, 1, bz, B::WeaponRack);
    bp.set(c - 3, 1, bz, B::Crate);
    ctx.point("jefe", static_cast<float>(c), 1, static_cast<float>(bz), 0, boss);
    ctx.point("cofre", static_cast<float>(c - 2), 1, static_cast<float>(bz + 1), 0, "3");
    // Tiendas pequenas en corro mirando al centro.
    static constexpr float kAngles[] = {0.0f, 180.0f, 40.0f, 140.0f, -35.0f, -145.0f};
    const float tr = static_cast<float>(R) * 0.62f;
    int ti = 0;
    for (float deg : kAngles) {
        const float a = deg * kPi / 180.0f;
        const int x = c + static_cast<int>(std::lround(std::cos(a) * tr)), z = c + static_cast<int>(std::lround(std::sin(a) * tr));
        const BlockId cloth = ti % 3 == 2 ? B::ClothRed : B::HideTent;
        coneTent(ctx, x, 0, z, 3, 5, cloth, static_cast<float>(c - x), static_cast<float>(c - z));
        if (ti < 2) ctx.point("cofre", static_cast<float>(x), 1, static_cast<float>(z), 0, ti == 0 ? "2" : "1");
        else ctx.point("spawner", static_cast<float>(x + (x < c ? 2 : -2)), 1, static_cast<float>(z + (z < c ? 2 : -2)), 0, ti % 2 ? grunt : archer);
        ++ti;
    }
    // Braseros sobre postes y jaulas con huesos.
    for (int i = 0; i < 8; ++i) {
        const float a = (static_cast<float>(i) + 0.5f) * kPi / 4.0f;
        const int x = c + static_cast<int>(std::lround(std::cos(a) * (static_cast<float>(R) - 2.5f)));
        const int z = c + static_cast<int>(std::lround(std::sin(a) * (static_cast<float>(R) - 2.5f)));
        if (bp.get(x, 1, z) != B::Air) continue;
        bp.set(x, 1, z, B::SpruceLog);
        bp.set(x, 2, z, B::Brazier);
    }
    for (int k = 0; k < 2; ++k) {
        const int ox = k == 0 ? c - 9 : c + 7, oz = c + 2;
        for (int z = oz; z <= oz + 2; ++z)
            for (int x = ox; x <= ox + 2; ++x) {
                const bool ring = x == ox || x == ox + 2 || z == oz || z == oz + 2;
                if (dist(x, z) > static_cast<float>(R) - 2.0f) continue;
                bp.set(x, 1, z, ring ? B::IronBars : B::Bones);
                bp.set(x, 2, z, ring ? B::IronBars : B::Air);
                bp.set(x, 3, z, B::SpruceLog);
            }
    }
    // Guarnicion.
    ctx.point("spawner", static_cast<float>(c - 3), 1, static_cast<float>(c - 3), 0, grunt);
    ctx.point("spawner", static_cast<float>(c + 3), 1, static_cast<float>(c - 3), 0, grunt);
    ctx.point("spawner", static_cast<float>(c), 1, static_cast<float>(gz + 3), 0, grunt);
    ctx.point("spawner", static_cast<float>(c - 4), 1, static_cast<float>(tz + 2), 0, grunt);
    ctx.point("spawner", static_cast<float>(c + 4), 1, static_cast<float>(tz + 2), 0, archer);
}

// --- Castillo no muerto (nigromante) ------------------------------------------------------

void undeadCastle(BuildContext& ctx, int W, int D, const std::vector<std::string>& garrison, const std::string& boss) {
    W |= 1;
    ctx.bp = Blueprint(W, 32, D);
    Blueprint& bp = ctx.bp;
    const int c = W / 2;
    clearBox(bp, 2, 1, 2, W - 3, 12, D - 3);
    const BlockId soul = B::SoulBrazier;
    // Patio: grava, roca y tierra.
    for (int z = 2; z < D - 2; ++z)
        for (int x = 2; x < W - 2; ++x) {
            const float r = ctx.rand(x, 0, z);
            if (r < 0.3f) bp.set(x, 0, z, B::Gravel);
            else if (r < 0.45f) bp.set(x, 0, z, B::Cobblestone);
            else if (r < 0.6f) bp.set(x, 0, z, B::DirtPath);
        }
    // Muralla negra (2 de grueso), rota a trozos.
    for (int z = 0; z < D; ++z)
        for (int x = 0; x < W; ++x) {
            const int edge = std::min(std::min(x, W - 1 - x), std::min(z, D - 1 - z));
            if (edge > 1) continue;
            int top = 10;
            if (ctx.rand(x / 3, 5, z / 3) < 0.3f) top -= 1 + static_cast<int>(ctx.rand(x, 6, z) * 4.0f);
            for (int y = 0; y < top; ++y) bp.set(x, y, z, darkStone(ctx, x, y, z));
            if (edge == 0 && (x + z) % 2 == 0 && top == 10) bp.set(x, top, z, B::BlackstoneBricks);
            if (edge == 1 && top == 10 && (x + z) % 11 == 0) bp.set(x, top, z, soul);
        }
    // Torres de las esquinas (7 x 7), coronas rotas.
    const int corners[4][2] = {{0, 0}, {W - 7, 0}, {0, D - 7}, {W - 7, D - 7}};
    int ci = 0;
    for (const auto& corner : corners) {
        const int ox = corner[0], oz = corner[1];
        const int th = 14 + static_cast<int>(ctx.rand(ci, 9) * 4.0f);
        for (int z = 0; z < 7; ++z)
            for (int x = 0; x < 7; ++x) {
                const bool ring = x == 0 || x == 6 || z == 0 || z == 6;
                for (int y = 0; y <= th; ++y) {
                    BlockId m = ring ? darkStone(ctx, ox + x, y, oz + z) : (y == 0 ? B::DungeonBricks : B::Air);
                    if (ring && (x == 3 || z == 3) && y % 6 == 3) m = B::IronBars;
                    if (y == th - 1 && !ring) m = B::BlackstoneBricks;  // azotea
                    bp.set(ox + x, y, oz + z, m);
                }
                if (ring && (x + z) % 2 == 0 && ctx.rand(ox + x, th, oz + z) < 0.7f) bp.set(ox + x, th + 1, oz + z, B::BlackstoneBricks);
            }
        // Puerta hacia el patio.
        const int dx = ox == 0 ? 6 : 0, dz = oz == 0 ? 6 : 0;
        (void)dx;
        const int door_x = ox + 3, door_z = oz + (dz ? 6 : 0);
        bp.set(door_x, 1, door_z, B::Air);
        bp.set(door_x, 2, door_z, B::Air);
        bp.set(ox + 3, th, oz + 3, soul);
        bp.set(ox + 2, 1, oz + 2, B::Cobweb);
        bp.set(ox + 4, 1, oz + 4, B::Bones);
        ctx.point("spawner", static_cast<float>(ox + 3), static_cast<float>(th), static_cast<float>(oz + 2), 0, pickTag(garrison, 3 + ci));
        ++ci;
    }
    ctx.point("cofre", 3, 1, 3, 0, "2");
    // Puerta principal: rastrillo medio bajado y dos torres que la flanquean.
    for (int z = 0; z <= 1; ++z)
        for (int x = c - 2; x <= c + 2; ++x) {
            bp.set(x, 0, z, B::DungeonBricks);
            for (int y = 1; y <= 6; ++y) bp.set(x, y, z, B::Air);
        }
    for (int x = c - 2; x <= c + 2; ++x) {
        bp.set(x, 6, 0, B::IronBars);
        bp.set(x, 5, 0, B::IronBars);
    }
    for (int side = -1; side <= 1; side += 2) {
        const int x0 = side < 0 ? c - 7 : c + 3;
        for (int z = 0; z <= 4; ++z)
            for (int x = x0; x <= x0 + 4; ++x) {
                const bool ring = x == x0 || x == x0 + 4 || z == 0 || z == 4;
                for (int y = 0; y <= 13; ++y) bp.set(x, y, z, ring || y == 0 || y == 12 ? darkStone(ctx, x, y, z) : B::Air);
                if (ring && (x + z) % 2 == 0) bp.set(x, 14, z, B::BlackstoneBricks);
            }
        bp.set(x0 + 2, 13, 2, soul);
        for (int y = 6; y <= 10; ++y) bp.set(x0 + 2, y, 0, B::BannerSkull);
    }
    // Camino central de losas oscuras con braseros de alma.
    const int kx0 = 8, kx1 = W - 9, kz1 = D - 4;
    const int kz0 = std::max(D / 2 - 2, kz1 - 20);
    for (int z = 2; z < kz0; ++z) {
        for (int x = c - 1; x <= c + 1; ++x) bp.set(x, 0, z, B::DungeonBricks);
        if (z % 6 == 3) {
            bp.set(c - 3, 1, z, soul);
            bp.set(c + 3, 1, z, soul);
        }
    }
    // Cementerio a los lados del camino: lapidas, ataudes y arboles muertos.
    for (int z = 8; z < kz0 - 2; z += 3)
        for (int x = 4; x < W - 4; x += 3) {
            if (std::abs(x - c) <= 4) continue;
            if ((x < 8 || x > W - 9) && z < 9) continue;
            const float r = ctx.rand(x, 11, z);
            if (r < 0.62f) grave(ctx, x, 0, z, 13);
            else if (r < 0.72f) bp.set(x, 0, z, B::Coffin);
            else if (r < 0.82f) put(bp, x, 1, z, B::Cobweb);
        }
    deadTree(ctx, 6, 0, kz0 - 3, 21);
    deadTree(ctx, W - 7, 0, kz0 - 3, 22);
    deadTree(ctx, c - 7, 0, 10, 23);
    ctx.point("cofre", static_cast<float>(c + 6), 1, static_cast<float>(kz0 - 2), 0, "2");
    // Torre del homenaje: dos plantas, azotea almenada y aguja de obsidiana.
    const int kc = (kx0 + kx1) / 2;
    const int f2 = 7, roof = 14;
    for (int z = kz0; z <= kz1; ++z)
        for (int x = kx0; x <= kx1; ++x) {
            const bool wall = x == kx0 || x == kx1 || z == kz0 || z == kz1;
            bp.set(x, 0, z, wall ? B::BlackstoneBricks : B::DungeonBricks);
            for (int y = 1; y <= roof; ++y) {
                BlockId m = B::Air;
                if (wall) {
                    m = darkStone(ctx, x, y, z);
                    const int along = (x == kx0 || x == kx1) ? z - kz0 : x - kx0;
                    if (along % 4 == 2 && ((y >= 2 && y <= 4) || (y >= f2 + 2 && y <= f2 + 4))) m = B::IronBars;
                    if (y == f2 || y == roof) m = B::CarvedStone;
                } else if (y == f2 || y == roof) {
                    m = B::BlackstoneBricks;
                }
                bp.set(x, y, z, m);
            }
            if (wall && (x + z) % 2 == 0) bp.set(x, roof + 1, z, B::BlackstoneBricks);
        }
    for (int x = kc - 1; x <= kc + 1; ++x)
        for (int y = 1; y <= 4; ++y) bp.set(x, y, kz0, B::Air);
    for (int y = 2; y <= 6; ++y) {
        bp.set(kc - 3, y, kz0, B::BannerPurple);
        bp.set(kc + 3, y, kz0, B::BannerPurple);
    }
    // Aguja central.
    const int sx = kc, sz = (kz0 + kz1) / 2;
    for (int y = roof + 1; y <= roof + 9; ++y)
        for (int z = sz - 2; z <= sz + 2; ++z)
            for (int x = sx - 2; x <= sx + 2; ++x) {
                const bool ring = std::abs(x - sx) == 2 || std::abs(z - sz) == 2;
                bp.set(x, y, z, ring ? darkStone(ctx, x, y, z) : B::Air);
            }
    for (int k = 0; k <= 4; ++k)
        for (int z = sz - 3 + k; z <= sz + 3 - k; ++z)
            for (int x = sx - 3 + k; x <= sx + 3 - k; ++x) bp.set(x, roof + 10 + k, z, B::Obsidian);
    bp.set(sx, roof + 15, sz, soul);
    // Salon del trono: columnas, braseros, ataudes, alfombra y el trono en su estrado.
    for (int z = kz0 + 1; z < kz1; ++z) bp.set(kc, 0, z, B::RugRed);
    for (int z = kz0 + 3; z < kz1 - 3; z += 4) {
        for (int y = 1; y < f2; ++y) {
            bp.set(kx0 + 3, y, z, B::BlackstoneBricks);
            bp.set(kx1 - 3, y, z, B::BlackstoneBricks);
        }
        bp.set(kx0 + 3, 1, z + 1, soul);
        bp.set(kx1 - 3, 1, z + 1, soul);
        bp.set(kx0 + 1, 1, z, B::Coffin);
        bp.set(kx1 - 1, 1, z, B::Coffin);
    }
    for (int z = kz1 - 3; z < kz1; ++z)
        for (int x = kc - 3; x <= kc + 3; ++x) bp.set(x, 1, z, B::CarvedStone);
    bp.set(kc, 2, kz1 - 1, B::Throne);
    bp.set(kc - 2, 2, kz1 - 1, B::SkullPile);
    bp.set(kc + 2, 2, kz1 - 1, B::SkullPile);
    bp.set(kc - 3, 2, kz1 - 2, B::Candles);
    bp.set(kc + 3, 2, kz1 - 2, B::Candles);
    for (int y = 2; y <= 5; ++y) {
        bp.set(kc - 2, y, kz1, B::BannerPurple);
        bp.set(kc + 2, y, kz1, B::BannerPurple);
        bp.set(kc, y + 1, kz1, B::BannerSkull);
    }
    bp.set(kc, f2 - 1, (kz0 + kz1) / 2, chandelierBlock());
    bp.set(kc, f2 - 1, kz0 + 3, chandelierBlock());
    bp.set(kx0 + 2, 1, kz0 + 1, B::Cobweb);
    bp.set(kx1 - 2, 3, kz1 - 1, B::Cobweb);
    ctx.point("jefe", static_cast<float>(kc), 1, static_cast<float>(kz1 - 5), 0, boss);
    ctx.point("spawner", static_cast<float>(kc - 3), 1, static_cast<float>(kz0 + 3), 0, pickTag(garrison, 2));
    ctx.point("spawner", static_cast<float>(kc + 3), 1, static_cast<float>(kz0 + 3), 0, pickTag(garrison, 0));
    // Escalera a la planta de arriba (muro izquierdo) y la planta: biblioteca prohibida.
    stairs(bp, kx0 + 1, 0, kz0 + 2, f2, 0, 1, B::BlackstoneBricks);
    const int y1 = f2 + 1;
    for (int z = kz0 + 2; z < kz1 - 1; z += 2) {
        put(bp, kx1 - 1, y1, z, B::Bookshelf);
        put(bp, kx1 - 1, y1 + 1, z, B::Bookshelf);
    }
    put(bp, kc, y1, kz1 - 2, B::Altar);
    put(bp, kc, y1 + 1, kz1 - 2, B::Candles);
    put(bp, kc - 2, y1, kz1 - 2, B::Coffin);
    put(bp, kc + 2, y1, kz1 - 2, soul);
    put(bp, kc + 3, y1, kz0 + 2, B::Cobweb);
    put(bp, kc, y1 + 4, (kz0 + kz1) / 2, chandelierBlock());
    ctx.point("cofre", static_cast<float>(kc + 1), static_cast<float>(y1), static_cast<float>(kz1 - 3), 0, "3");
    ctx.point("spawner", static_cast<float>(kc), static_cast<float>(y1), static_cast<float>(kz0 + 4), 0, pickTag(garrison, 1));
    // Guarnicion del patio y la puerta.
    ctx.point("spawner", static_cast<float>(c - 6), 1, 8, 0, pickTag(garrison, 0));
    ctx.point("spawner", static_cast<float>(c + 6), 1, 8, 0, pickTag(garrison, 0));
    ctx.point("spawner", static_cast<float>(c - 8), 1, static_cast<float>(kz0 - 5), 0, pickTag(garrison, 1));
    ctx.point("spawner", static_cast<float>(c + 8), 1, static_cast<float>(kz0 - 5), 0, pickTag(garrison, 2));
    ctx.point("spawner", static_cast<float>(c), 1, 4, 0, pickTag(garrison, 2));
}

// --- Ciudad maldita (en ruinas) -----------------------------------------------------------

void cursedCity(BuildContext& ctx, int S, const std::vector<std::string>& garrison, const std::string& boss) {
    S |= 1;
    ctx.bp = Blueprint(S, 18, S);
    Blueprint& bp = ctx.bp;
    const int c = S / 2;
    clearBox(bp, 2, 1, 2, S - 3, 8, S - 3);
    for (int z = 2; z < S - 2; ++z)
        for (int x = 2; x < S - 2; ++x) {
            const float r = ctx.rand(x, 0, z);
            if (r < 0.2f) bp.set(x, 0, z, B::Gravel);
            else if (r < 0.3f) bp.set(x, 0, z, B::DirtPath);
            else if (r < 0.34f) put(bp, x, 1, z, ctx.rand(z, 1, x) < 0.5f ? B::DeadBush : B::Bones);
        }
    // Calles en cruz de adoquin roto.
    for (int z = 0; z < S; ++z)
        for (int x = 0; x < S; ++x) {
            if (std::abs(x - c) > 2 && std::abs(z - c) > 2) continue;
            const float r = ctx.rand(x, 1, z);
            bp.set(x, 0, z, r < 0.5f ? B::Cobblestone : (r < 0.75f ? B::Gravel : (r < 0.9f ? B::CrackedBricks : B::Paving)));
        }
    // Muralla rota con brechas (y las cuatro puertas abiertas).
    for (int z = 0; z < S; ++z)
        for (int x = 0; x < S; ++x) {
            const int edge = std::min(std::min(x, S - 1 - x), std::min(z, S - 1 - z));
            if (edge > 1) continue;
            if (std::abs(x - c) <= 2 || std::abs(z - c) <= 2) continue;
            const int h = static_cast<int>(ctx.rand(x / 4, 2, z / 4) * 7.0f + ctx.rand(x, 3, z) * 2.0f) - 1;
            for (int y = 0; y <= h; ++y) bp.set(x, y, z, ruinStone(ctx, x, y, z));
        }
    // Casas en ruinas en lotes de 12 x 12 (fuera de las calles, la plaza y la catedral).
    const int cath_x0 = c - 8, cath_x1 = c + 8, cath_z1 = S - 5, cath_z0 = std::max(c + 8, cath_z1 - 24);
    std::size_t spawn_i = 0;
    int lot_i = 0;
    for (int gz = 3; gz + 11 < S - 3; gz += 12)
        for (int gx = 3; gx + 11 < S - 3; gx += 12) {
            const int mx = gx + 6, mz = gz + 6;
            if (std::abs(mx - c) < 9 || std::abs(mz - c) < 9) continue;
            if (gx + 11 >= cath_x0 - 1 && gx <= cath_x1 + 1 && gz + 11 >= cath_z0 - 1) continue;
            ++lot_i;
            const int w = ctx.randInt(6, 9, gx, gz), d = ctx.randInt(6, 9, gz, gx);
            const int x0 = gx + 1 + ctx.randInt(0, 10 - w, gx + 1, gz), z0 = gz + 1 + ctx.randInt(0, 10 - d, gx, gz + 1);
            const int x1 = x0 + w - 1, z1 = z0 + d - 1;
            const int base_h = ctx.randInt(2, 6, gx + 3, gz + 3);
            // Puerta hacia la calle mas cercana.
            const bool door_x_side = std::abs(mx - c) < std::abs(mz - c);
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) {
                    const bool wall = x == x0 || x == x1 || z == z0 || z == z1;
                    if (!wall) {
                        if (ctx.rand(x, 4, z) < 0.5f) bp.set(x, 0, z, B::DarkPlanks);
                        continue;
                    }
                    const int h = std::max(0, base_h + static_cast<int>(ctx.rand(x, 5, z) * 3.0f) - 1);
                    bool door = false;
                    if (door_x_side) door = (mx < c ? x == x1 : x == x0) && z == (z0 + z1) / 2;
                    else door = (mz < c ? z == z1 : z == z0) && x == (x0 + x1) / 2;
                    bp.set(x, 0, z, ruinStone(ctx, x, 0, z));
                    for (int y = 1; y <= h; ++y) {
                        if (door && y <= 2) continue;
                        const int along = (x == x0 || x == x1) ? z - z0 : x - x0;
                        if (along % 3 == 1 && y == 2) continue;  // ventana
                        bp.set(x, y, z, ruinStone(ctx, x, y, z));
                    }
                    if (h >= 5 && ctx.rand(x, 6, z) < 0.5f) bp.set(x, h + 1, z, B::RoofSlate);
                }
            // Escombros dentro.
            put(bp, x0 + 1, 1, z0 + 1, B::Cobweb);
            put(bp, x1 - 1, 1, z1 - 1, ctx.rand(lot_i, 7) < 0.5f ? B::Crate : B::Barrel);
            put(bp, (x0 + x1) / 2, 1, z1 - 1, B::Bones);
            if (ctx.rand(lot_i, 8) < 0.25f) deadTree(ctx, (x0 + x1) / 2, 0, (z0 + z1) / 2, lot_i);
            if (lot_i % 4 == 1) ctx.point("cofre", static_cast<float>(x0 + 1), 1, static_cast<float>(z1 - 1), 0, ctx.rand(lot_i, 9) < 0.3f ? "2" : "1");
            if (lot_i % 2 == 0)
                ctx.point("spawner", static_cast<float>((x0 + x1) / 2), 1, static_cast<float>((z0 + z1) / 2), 0, pickTag(garrison, spawn_i++));
        }
    // Plaza: fuente seca, arbol muerto y lapidas.
    for (int z = c - 7; z <= c + 7; ++z)
        for (int x = c - 7; x <= c + 7; ++x) {
            const float d = std::hypot(static_cast<float>(x - c), static_cast<float>(z - c));
            if (d > 7.3f) continue;
            bp.set(x, 0, z, ctx.rand(x, 9, z) < 0.7f ? B::Paving : B::CrackedBricks);
            if (d < 3.5f && d > 2.5f) {
                bp.set(x, 1, z, ctx.rand(x, 10, z) < 0.8f ? B::Marble : B::CrackedBricks);
            } else if (d <= 2.5f) {
                bp.set(x, 0, z, B::Gravel);
                put(bp, x, 1, z, ctx.rand(x, 11, z) < 0.3f ? B::Bones : B::Air);
            }
        }
    for (int y = 1; y <= 3; ++y) bp.set(c, y, c, y == 3 ? B::CrackedBricks : B::Marble);
    deadTree(ctx, c + 5, 0, c - 5, 31);
    grave(ctx, c - 5, 0, c + 5, 41);
    grave(ctx, c - 6, 0, c + 3, 42);
    for (int i = 0; i < 4; ++i) {
        const int x = c + (i % 2 ? 6 : -6), z = c + (i < 2 ? -6 : 6);
        if (bp.get(x, 1, z) == B::Air || bp.get(x, 1, z) == kKeep) bp.set(x, 1, z, B::SoulBrazier);
    }
    // Braseros de alma a lo largo de las calles.
    for (int t = 10; t < c - 2; t += 9) {
        for (int k = 0; k < 4; ++k) {
            const int x = k == 0 ? c + t : (k == 1 ? c - t : c + (k == 2 ? 3 : -3));
            const int z = k < 2 ? c + 3 : (k == 2 ? c + t : c - t);
            put(bp, x, 1, z, B::SoulBrazier);
        }
    }
    // Catedral en ruinas al fondo: naves con columnas rotas, bancos, altar y el jefe.
    if (cath_z1 - cath_z0 >= 14) {
        for (int z = cath_z0; z <= cath_z1; ++z)
            for (int x = cath_x0; x <= cath_x1; ++x) {
                const bool wall = x == cath_x0 || x == cath_x1 || z == cath_z0 || z == cath_z1;
                bp.set(x, 0, z, wall ? B::StoneBricks : (ctx.rand(x, 12, z) < 0.8f ? B::Paving : B::CrackedBricks));
                for (int y = 1; y <= 14; ++y) bp.set(x, y, z, B::Air);
                if (!wall) continue;
                const int h = 6 + static_cast<int>(ctx.rand(x / 2, 13, z / 2) * 8.0f);
                const bool door = z == cath_z0 && std::abs(x - c) <= 1;
                for (int y = 1; y <= h; ++y) {
                    if (door && y <= 4) continue;
                    const int along = (x == cath_x0 || x == cath_x1) ? z - cath_z0 : x - cath_x0;
                    if (along % 4 == 2 && y >= 3 && y <= 7) continue;  // ventanales
                    bp.set(x, y, z, ruinStone(ctx, x, y, z));
                }
            }
        for (int z = cath_z0 + 3; z < cath_z1 - 4; z += 4)
            for (int side = -1; side <= 1; side += 2) {
                const int x = c + side * 4;
                const int h = 2 + static_cast<int>(ctx.rand(x, 14, z) * 8.0f);
                for (int y = 1; y <= h; ++y) bp.set(x, y, z, B::MarblePillar);
                for (int bx = c - 3; bx <= c + 3; ++bx)
                    if (std::abs(bx - c) >= 2 && ctx.rand(bx, 15, z) < 0.6f) bp.set(bx, 1, z + 2, B::Bench);
            }
        bp.set(c, 1, cath_z1 - 2, B::Altar);
        bp.set(c, 2, cath_z1 - 2, B::Candles);
        bp.set(c - 2, 1, cath_z1 - 2, B::SoulBrazier);
        bp.set(c + 2, 1, cath_z1 - 2, B::SoulBrazier);
        for (int y = 3; y <= 7; ++y) {
            bp.set(c - 3, y, cath_z1, B::BannerPurple);
            bp.set(c + 3, y, cath_z1, B::BannerPurple);
        }
        put(bp, cath_x0 + 1, 1, cath_z1 - 1, B::Cobweb);
        put(bp, cath_x1 - 1, 1, cath_z1 - 1, B::Cobweb);
        ctx.point("jefe", static_cast<float>(c), 1, static_cast<float>(cath_z1 - 6), 0, boss);
        ctx.point("cofre", static_cast<float>(c + 1), 1, static_cast<float>(cath_z1 - 1), 0, "3");
        ctx.point("spawner", static_cast<float>(c), 1, static_cast<float>(cath_z0 + 3), 0, pickTag(garrison, spawn_i++));
    } else {
        ctx.point("jefe", static_cast<float>(c), 1, static_cast<float>(c + 4), 0, boss);
    }
    // Guarnicion de la plaza y las puertas.
    ctx.point("spawner", static_cast<float>(c - 4), 1, static_cast<float>(c), 0, pickTag(garrison, spawn_i++));
    ctx.point("spawner", static_cast<float>(c + 4), 1, static_cast<float>(c), 0, pickTag(garrison, spawn_i++));
    ctx.point("spawner", static_cast<float>(c), 1, 5, 0, pickTag(garrison, spawn_i++));
    ctx.point("spawner", 5, 1, static_cast<float>(c), 0, pickTag(garrison, spawn_i++));
    ctx.point("spawner", static_cast<float>(S - 6), 1, static_cast<float>(c), 0, pickTag(garrison, spawn_i++));
}

// --- Torre del culto -----------------------------------------------------------------------

void cultistTower(BuildContext& ctx, int floors, const std::string& minion, const std::string& boss) {
    constexpr int S = 25, T = 11, fh = 6;
    const int D = 33;
    const int top = floors * fh;
    ctx.bp = Blueprint(S, top + 18, D);
    Blueprint& bp = ctx.bp;
    const int c = S / 2;
    const int rz = 11;  // centro del circulo ritual
    const int tz0 = D - T - 1, tz1 = D - 2, tx0 = c - T / 2, tx1 = c + T / 2;
    const int tcz = (tz0 + tz1) / 2;
    // Circulo ritual delante: anillo de runas, obsidiana y basalto, velas, braseros, estandartes y altar.
    for (int z = 0; z < tz0; ++z)
        for (int x = 0; x < S; ++x) {
            const float d = std::hypot(static_cast<float>(x - c), static_cast<float>(z - rz));
            if (d > 11.5f && std::abs(x - c) > 1) continue;
            for (int y = 1; y <= 6; ++y) bp.set(x, y, z, B::Air);
            if (d > 11.5f) {
                bp.set(x, 0, z, B::Basalt);  // camino
                continue;
            }
            BlockId g = static_cast<int>(d) % 2 ? B::Obsidian : B::Basalt;
            if (d > 9.5f && d <= 10.5f) g = B::RuneStone;
            bp.set(x, 0, z, g);
            if (d > 6.6f && d < 7.4f && (x + z) % 2 == 0) bp.set(x, 1, z, B::Candles);
        }
    for (int i = 0; i < 4; ++i) {
        const float a = static_cast<float>(i) * kPi * 0.5f;
        bp.set(c + static_cast<int>(std::lround(std::cos(a) * 9.0f)), 1, rz + static_cast<int>(std::lround(std::sin(a) * 9.0f)), B::SoulBrazier);
        const float b = a + kPi * 0.25f;
        const int px = c + static_cast<int>(std::lround(std::cos(b) * 9.0f)), pz = rz + static_cast<int>(std::lround(std::sin(b) * 9.0f));
        for (int y = 1; y <= 3; ++y) bp.set(px, y, pz, B::Fence);
        bp.set(px, 4, pz, B::BannerPurple);
        bp.set(px, 5, pz, B::BannerPurple);
    }
    bp.set(c, 1, rz, B::Altar);
    bp.set(c, 2, rz, B::Candles);
    put(bp, c - 1, 1, rz + 1, B::SkullPile);
    put(bp, c + 1, 1, rz + 1, B::Bones);
    ctx.point("jefe", static_cast<float>(c), 1, static_cast<float>(rz - 3), 0, boss);
    ctx.point("spawner", static_cast<float>(c - 5), 1, static_cast<float>(rz), 0, minion);
    ctx.point("spawner", static_cast<float>(c + 5), 1, static_cast<float>(rz), 0, minion);
    ctx.point("spawner", static_cast<float>(c), 1, static_cast<float>(rz + 6), 0, minion);
    // La torre: piedra negra con esquinas de obsidiana, pisos de 6.
    const int ttop = top + 1;
    for (int z = tz0; z <= tz1; ++z)
        for (int x = tx0; x <= tx1; ++x) {
            const bool ex = x == tx0 || x == tx1, ez = z == tz0 || z == tz1;
            bp.set(x, 0, z, ex || ez ? B::Obsidian : B::BlackstoneBricks);
            for (int y = 1; y <= ttop; ++y) {
                BlockId m = B::Air;
                if (ex || ez) {
                    m = (ex && ez) ? B::Obsidian : darkStone(ctx, x, y, z);
                    const int along = ex ? z - tz0 : x - tx0;
                    if (along == T / 2 && (y % fh == 3 || y % fh == 4) && y > fh) m = B::IronBars;
                    if (y % fh == 0) m = B::Obsidian;
                } else if (y % fh == 0) {
                    m = B::BlackstoneBricks;
                }
                bp.set(x, y, z, m);
            }
            if ((ex || ez) && (x + z) % 2 == 0) bp.set(x, ttop + 1, z, B::Obsidian);
        }
    // Puerta con marco de obsidiana.
    for (int y = 1; y <= 3; ++y) bp.set(c, y, tz0, B::Air);
    bp.set(c - 1, 2, tz0 - 1, B::SoulBrazier);
    bp.set(c + 1, 2, tz0 - 1, B::SoulBrazier);
    // Escalera de caracol alrededor de una columna (como la torre de mago).
    static constexpr int kRing[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}};
    for (int y = 1; y <= top + 1; ++y) bp.set(c, y, tcz, B::Obsidian);
    for (int y = 1; y <= top; ++y)
        for (int i = 0; i < 8; ++i) {
            const int x = c + kRing[i][0], z = tcz + kRing[i][1];
            const bool step = ((y - 1) % 8) == i && y <= top - fh + 1 + 7;
            bp.set(x, y, z, step ? B::BlackstoneBricks : B::Air);
        }
    // Pisos: jaulas, estanterias, ataudes, braseros y estandartes.
    for (int f = 0; f < floors; ++f) {
        const int y = f * fh + 1;
        const bool last = f == floors - 1;
        put(bp, tx0 + 1, y, tz0 + 1, B::SoulBrazier);
        put(bp, tx1 - 1, y, tz1 - 1, B::SoulBrazier);
        put(bp, c, y + 4, tz1 - 2, chandelierBlock());
        if (f == 0) {
            for (int x = tx0 + 1; x <= tx1 - 1; x += 2) put(bp, x, y, tz1 - 1, B::Coffin);
            put(bp, tx1 - 1, y, tz0 + 2, B::Barrel);
        } else if (last) {
            put(bp, tx0 + 1, y, tz1 - 1, B::Altar);
            put(bp, tx0 + 1, y + 1, tz1 - 1, B::Candles);
            put(bp, tx1 - 1, y, tz0 + 1, B::GoldBlock);
            for (int x = tx0 + 2; x <= tx1 - 2; x += 2) put(bp, x, y, tz1 - 1, B::Bookshelf);
            ctx.point("cofre", static_cast<float>(tx1 - 2), static_cast<float>(y), static_cast<float>(tz1 - 1), 0, "3");
        } else if (f % 2 == 1) {
            for (int z = tz0 + 1; z <= tz1 - 1; ++z) {
                put(bp, tx0 + 1, y, z, B::Bookshelf);
                put(bp, tx0 + 1, y + 1, z, B::Bookshelf);
            }
            put(bp, tx1 - 1, y, tcz, B::Table);
            put(bp, tx1 - 1, y + 1, tcz, B::Candles);
        } else {
            for (int z = tz0 + 1; z <= tz0 + 3; ++z)
                for (int x = tx1 - 3; x <= tx1 - 1; ++x) {
                    const bool ring = x == tx1 - 3 || z == tz0 + 3;
                    put(bp, x, y, z, ring ? B::IronBars : B::Bones);
                    put(bp, x, y + 1, z, ring ? B::IronBars : B::Air);
                }
        }
        for (int yy = y + 1; yy <= y + 3; ++yy) {
            if (bp.get(tx0, yy, tcz) != B::IronBars) bp.set(tx0, yy, tcz + 2, B::BannerPurple);
        }
        if (f > 0 && !last) ctx.point("spawner", static_cast<float>(tx1 - 2), static_cast<float>(y), static_cast<float>(tcz), 0, minion);
    }
    // Aguja de obsidiana con un brasero de alma en la punta.
    for (int k = 0; k <= 5; ++k)
        for (int z = tcz - 4 + k; z <= tcz + 4 - k; ++z)
            for (int x = c - 4 + k; x <= c + 4 - k; ++x) {
                const bool ring = std::abs(x - c) == 4 - k || std::abs(z - tcz) == 4 - k;
                bp.set(x, ttop + 2 + k * 2, z, ring ? B::Obsidian : B::Air);
                bp.set(x, ttop + 3 + k * 2, z, ring ? B::BlackstoneBricks : B::Air);
            }
    bp.set(c, ttop + 14, tcz, B::SoulBrazier);
}

}  // namespace build

}  // namespace cramion::voxel
