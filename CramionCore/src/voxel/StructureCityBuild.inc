// CIUDADES-GRANDES: piezas de las metropolis, castillos y misterios. Se incluye al final de
// StructureBuild.cpp (dentro de namespace build) para usar sus ayudas internas (Palette,
// houseShell, Reserve, place, stairsZ, furnishLiving, furnishBedroom). Fachada hacia -z.
//
// Cofres de botin: bloque Chest + punto "cofre_botin" (tag "<nivel>:<tabla>"); notas: bloque
// LoreTablet + punto "nota" (tag = tema). Pasadizos: block::SecretBookshelf / SecretWall (no chocan).

namespace {

// Copia `src` dentro de `dst` desplazada (las celdas kKeep no pisan).
void blit(Blueprint& dst, const Blueprint& src, int ox, int oy, int oz) {
    for (int y = 0; y < src.h; ++y)
        for (int z = 0; z < src.d; ++z)
            for (int x = 0; x < src.w; ++x) {
                const BlockId id = src.cells[src.index(x, y, z)];
                if (id != kKeep) dst.set(x + ox, y + oy, z + oz, id);
            }
}

// Mueve los puntos ya puestos (despues de componer el plano en otro mayor).
void shiftPoints(BuildContext& ctx, std::size_t from, float dx, float dy, float dz) {
    for (std::size_t i = from; i < ctx.points.size(); ++i) {
        ctx.points[i].x += dx;
        ctx.points[i].y += dy;
        ctx.points[i].z += dz;
    }
}

// Piedra vieja con musgo y grietas.
BlockId oldStone(const BuildContext& ctx, int x, int y, int z) {
    const float r = ctx.rand(x, y, z + 501);
    return r < 0.25f ? B::MossyBricks : (r < 0.35f ? B::CrackedBricks : (r < 0.5f ? B::Cobblestone : B::StoneBricks));
}

// Caja hueca de piedra (paredes, suelo y techo) con el interior de aire.
void stoneRoom(BuildContext& ctx, int x0, int y0, int z0, int x1, int y1, int z1, BlockId floor) {
    Blueprint& bp = ctx.bp;
    for (int y = y0; y <= y1; ++y)
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                const bool shell = x == x0 || x == x1 || y == y0 || y == y1 || z == z0 || z == z1;
                if (!shell) bp.set(x, y, z, B::Air);
                else if (y == y0) bp.set(x, y, z, floor);
                else bp.set(x, y, z, oldStone(ctx, x, y, z));
            }
}

// Tejado a cuatro aguas sobre un plano de casa (houseShell) desde y = top: anillos que se
// cierran hacia el centro; la chimenea sigue.
void hipRoof(BuildContext& ctx, const Shell& s, const Palette& p, int W, int D) {
    Blueprint& bp = ctx.bp;
    for (int y = s.top; y < bp.h; ++y)
        for (int z = 0; z < D; ++z)
            for (int x = 0; x < W; ++x) {
                const BlockId c = bp.get(x, y, z);
                if (y == s.top && c != kKeep && x >= s.x0 && x <= s.x1 && z >= s.z0 && z <= s.z1) continue;  // techo del ultimo piso
                if (c != kKeep && !(s.chimney && x == s.chimney_x && z == s.chimney_z)) bp.set(x, y, z, kKeep);
            }
    for (int k = 0;; ++k) {
        const int xa = k, xb = W - 1 - k, za = k, zb = D - 1 - k;
        if (xa > xb || za > zb) break;
        const int y = s.top + k;
        if (y >= bp.h) break;
        const bool last = xb - xa <= 1 || zb - za <= 1;
        for (int z = za; z <= zb; ++z)
            for (int x = xa; x <= xb; ++x) {
                if (s.chimney && x == s.chimney_x && z == s.chimney_z) continue;
                const bool ring = x == xa || x == xb || z == za || z == zb;
                if (ring || last) bp.set(x, y, z, last ? p.ridge : p.roof);
                else if (k > 0) bp.set(x, y, z, B::Air);
            }
        if (last) break;
    }
}

}  // namespace

void lootChest(BuildContext& ctx, int x, int y, int z, int dir, int tier, const std::string& table) {
    ctx.bp.set(x, y, z, B::Chest);
    ctx.point("cofre_botin", static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), dir, std::to_string(tier) + ":" + table);
}

void loreNote(BuildContext& ctx, int x, int y, int z, int dir, const std::string& topic) {
    ctx.bp.set(x, y, z, B::LoreTablet);
    ctx.point("nota", static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), dir, topic);
}

// --- Casa adosada de ciudad --------------------------------------------------------------

void townhouse(BuildContext& ctx, int w, int d, int floors, const std::string& npc_tag, int variant, bool cellar) {
    const Palette p = paletteFor(ctx, variant);
    Reserve* r = nullptr;
    std::unique_ptr<Reserve> holder;
    const bool chimney = ctx.style != BuildStyle::Desert && ctx.style != BuildStyle::Beach;
    Shell s = houseShell(ctx, r, holder, w, d, floors, p, chimney);
    const int W = w + 2, D = d + 2;
    cellar = cellar && d >= 8 && w >= 6;
    // Bajada a la bodega: franja junto al muro derecho (los muebles no la ocupan).
    if (cellar)
        for (int z = s.iz0; z <= s.iz0 + 5; ++z) r->markColumn(s.ix1, 1, 3, z);
    const RoomSpots living = furnishLiving(ctx, s, *r, 0, p, variant);
    if (variant % 3 != 2) ctx.point("npc", living.npc_x, 1, living.npc_z, 2, npc_tag);
    if (floors == 1) {
        furnishBedroom(ctx, s, *r, 0, p, variant, true);
    } else {
        for (int f = 1; f < floors; ++f) {
            const RoomSpots bed = furnishBedroom(ctx, s, *r, f, p, variant + f, false);
            if (bed.bed && f == 1) ctx.point("cama", static_cast<float>(bed.bed_x), static_cast<float>(4 * f + 2), static_cast<float>(bed.bed_z), 0);
        }
    }
    // Tejado a cuatro aguas en algunas (no en las de tejado plano ni nevado).
    if (!p.flat && !p.snowy && variant % 3 == 1) hipRoof(ctx, s, p, W, D);
    // Jardineras y toldo en la fachada.
    if (!p.flat) {
        for (int x = s.x0 + 2; x < s.x1; x += 4) {
            if (x == s.door_x || ctx.rand(x, 19) < 0.5f) continue;
            ctx.bp.set(x, 5, s.z0 - 1, floors > 1 ? B::Bush : kKeep);
        }
    }
    if (!cellar) return;
    // Bodega: el plano crece 5 por debajo (el suelo de la casa queda en y = 5).
    constexpr int g = 5;
    Blueprint big(W, ctx.bp.h + g, D);
    blit(big, ctx.bp, 0, g, 0);
    shiftPoints(ctx, 0, 0.0f, static_cast<float>(g), 0.0f);
    ctx.bp = std::move(big);
    stoneRoom(ctx, s.x0, 0, s.z0, s.x1, g, s.z1, B::Cobblestone);
    // El techo de la bodega es el suelo de la casa (lo vuelve a poner).
    for (int z = s.z0; z <= s.z1; ++z)
        for (int x = s.x0; x <= s.x1; ++x) {
            const bool ring = x == s.x0 || x == s.x1 || z == s.z0 || z == s.z1;
            ctx.bp.set(x, g, z, ring ? p.found : p.floor);
        }
    for (int j = 1; j <= g; ++j) {
        const int z = s.iz0 + j - 1;
        ctx.bp.set(s.ix1, g - j, z, B::StoneBricks);
        for (int y = g - j + 1; y <= g - j + 3; ++y) ctx.bp.set(s.ix1, y, z, B::Air);
    }
    if (s.chimney) ctx.bp.set(s.chimney_x, g, s.chimney_z, B::Bricks);
    // Barriles, cajas y el cofre de la bodega.
    for (int z = s.iz0 + 1; z <= s.iz1; z += 2) ctx.bp.set(s.ix0, 1, z, z % 4 == 1 ? B::Barrel : B::Crate);
    ctx.bp.set(s.ix0, 2, s.iz0 + 1, B::Barrel);
    ctx.bp.set((s.ix0 + s.ix1) / 2, 3, (s.iz0 + s.iz1) / 2, B::Lantern);
    lootChest(ctx, s.ix0 + 1, 1, s.iz1, 0, 1, "bodega");
    // A veces un escondite de contrabandistas tras un muro falso (fondo de la bodega).
    if (variant % 4 == 0 && s.ix1 - s.ix0 >= 4) {
        const int cx = (s.ix0 + s.ix1) / 2;
        ctx.bp.set(cx, 1, s.z1, B::SecretWall);
        ctx.bp.set(cx, 2, s.z1, B::SecretWall);
        // (la casa ocupa hasta z1; el escondite va bajo el alero de atras, z1 + 1)
        for (int x = cx - 1; x <= cx + 1; ++x)
            for (int y = 0; y <= 3; ++y) ctx.bp.set(x, y, s.z1 + 1, y == 0 || y == 3 || x != cx ? B::Cobblestone : B::Air);
        // Solo cabe una celda: el cofre queda en el hueco del muro falso (se ve al cruzarlo).
        ctx.bp.set(cx, 2, s.z1 + 1, B::Candles);
        lootChest(ctx, cx, 1, s.z1 + 1, 2, 2, "contrabando");
    }
}

// --- Catedral -------------------------------------------------------------------------------

void cathedral(BuildContext& ctx) {
    constexpr int W = 23, D = 45, g = kCryptDepth;
    constexpr int nave_h = 16, aisle_h = 9, tower_h = 26;
    ctx.bp = Blueprint(W, g + tower_h + 9, D);
    Blueprint& bp = ctx.bp;
    const int cx = W / 2;  // 11
    const int bz0 = 7, bz1 = D - 2;  // cuerpo (muros incluidos): z 7..43
    const int ax0 = 1, ax1 = W - 2;  // muros de fuera de las naves laterales
    const int px0 = 6, px1 = W - 7;  // filas de pilares (nave central entre medias: 7..15)
    const auto stone = [&](int x, int y, int z) { return ctx.rand(x, y, z) < 0.1f ? B::MossyBricks : B::StoneBricks; };
    // Cimiento macizo hasta el suelo de la cripta.
    for (int z = 0; z < D; ++z)
        for (int x = 0; x < W; ++x)
            for (int y = 0; y < g; ++y) bp.set(x, y, z, B::Stone);
    // Suelo de la iglesia.
    for (int z = bz0; z <= bz1; ++z)
        for (int x = ax0; x <= ax1; ++x) bp.set(x, g, z, (x + z) % 2 ? B::Paving : B::Marble);
    // Muros de las naves laterales (con vidrieras) y del fondo.
    for (int y = g + 1; y <= g + aisle_h; ++y)
        for (int z = bz0; z <= bz1; ++z)
            for (int x = ax0; x <= ax1; ++x) {
                const bool wall = x == ax0 || x == ax1 || z == bz0 || z == bz1;
                if (!wall) {
                    bp.set(x, y, z, B::Air);
                    continue;
                }
                BlockId m = stone(x, y, z);
                const int along = (x == ax0 || x == ax1) ? z - bz0 : x - ax0;
                if (along % 4 == 0 || y == g + 1) m = B::CarvedStone;
                if ((x == ax0 || x == ax1) && along % 4 == 2 && y >= g + 3 && y <= g + 6) m = (along / 4) % 2 ? B::StainedGlassBlue : B::StainedGlassRose;
                bp.set(x, y, z, m);
            }
    // Contrafuertes por fuera.
    for (int z = bz0 + 4; z < bz1; z += 4)
        for (int y = g + 1; y <= g + aisle_h - 2; ++y) {
            bp.set(0, y, z, B::StoneBricks);
            bp.set(W - 1, y, z, B::StoneBricks);
        }
    // Nave central: pilares, arcos y muros altos con ventanas (claristorio).
    for (int y = g + 1; y <= g + nave_h; ++y)
        for (int z = bz0; z <= bz1; ++z)
            for (int x = px0; x <= px1; ++x) {
                const bool side = x == px0 || x == px1;
                const bool end = z == bz0 || z == bz1;
                if (y <= g + aisle_h && !end) {
                    if (side && (z - bz0) % 4 == 0) bp.set(x, y, z, B::MarblePillar);
                    else if (side && y >= g + aisle_h - 1) bp.set(x, y, z, B::CarvedStone);  // arcos
                    else if (!side || y < g + aisle_h - 1) bp.set(x, y, z, B::Air);
                    continue;
                }
                if (side || end) {
                    BlockId m = stone(x, y, z);
                    if (side && (z - bz0) % 4 == 2 && y >= g + aisle_h + 2 && y <= g + nave_h - 3) m = B::StainedGlassBlue;
                    if (y == g + nave_h) m = B::CarvedStone;
                    bp.set(x, y, z, m);
                } else {
                    bp.set(x, y, z, B::Air);
                }
            }
    // Tejados de las naves laterales (a un agua, de pizarra) y de la central (a dos aguas).
    for (int z = bz0 - 1; z <= bz1 + 1; ++z) {
        for (int x = 0; x <= px0; ++x) bp.set(x, g + aisle_h + 1 + x / 2, z, B::RoofSlate);
        for (int x = px1; x < W; ++x) bp.set(x, g + aisle_h + 1 + (W - 1 - x) / 2, z, B::RoofSlate);
    }
    for (int k = 0;; ++k) {
        const int xa = px0 - 1 + k, xb = px1 + 1 - k;
        if (xa > xb) break;
        const int y = g + nave_h + 1 + k;
        for (int z = bz0 - 1; z <= bz1 + 1; ++z) {
            bp.set(xa, y, z, xa >= xb - 1 ? B::CarvedStone : ctx.roof);
            bp.set(xb, y, z, xa >= xb - 1 ? B::CarvedStone : ctx.roof);
            if (k > 0 && z >= bz0 && z <= bz1)
                for (int x = xa + 1; x < xb; ++x) bp.set(x, y, z, (z == bz0 || z == bz1) ? stone(x, y, z) : B::Air);
        }
    }
    // Fachada: portada, roseton y dos campanarios (z 0..6).
    for (int y = g + 1; y <= g + nave_h; ++y)
        for (int x = px0 + 1; x < px1; ++x) bp.set(x, y, bz0, stone(x, y, bz0));
    for (int x = cx - 1; x <= cx + 1; ++x)
        for (int y = g + 1; y <= g + 5; ++y) bp.set(x, y, bz0, B::Door);
    for (int x = cx - 2; x <= cx + 2; ++x) bp.set(x, g + 6, bz0, B::CarvedStone);
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            if (dx * dx + dy * dy <= 5) bp.set(cx + dx, g + 11 + dy, bz0, (dx + dy) % 2 ? B::StainedGlassRose : B::StainedGlassBlue);
    // Portico (escalinata y columnas) entre los campanarios.
    for (int z = 3; z < bz0; ++z)
        for (int x = px0 + 1; x < px1; ++x) {
            bp.set(x, g, z, B::Marble);
            for (int y = g + 1; y <= g + 7; ++y) bp.set(x, y, z, (z == 3 && (x - px0) % 2 == 1 && y < g + 7) ? B::MarblePillar : B::Air);
            bp.set(x, g + 7, z, B::CarvedStone);
        }
    bp.set(cx - 2, g + 1, 4, B::Brazier);
    bp.set(cx + 2, g + 1, 4, B::Brazier);
    const auto belfry = [&](int tx0) {
        const int tx1 = tx0 + 6, tz0 = 0, tz1 = 6;
        for (int y = g; y <= g + tower_h; ++y)
            for (int z = tz0; z <= tz1; ++z)
                for (int x = tx0; x <= tx1; ++x) {
                    const bool ring = x == tx0 || x == tx1 || z == tz0 || z == tz1;
                    if (y == g) {
                        bp.set(x, y, z, B::StoneBricks);
                        continue;
                    }
                    BlockId m = ring ? stone(x, y, z) : B::Air;
                    if (ring && (y - g) % 7 == 0) m = B::CarvedStone;
                    const bool mid = x == tx0 + 3 || z == tz0 + 3;
                    if (ring && mid && y >= g + tower_h - 6 && y <= g + tower_h - 3) m = B::Air;  // campanario abierto
                    if (ring && mid && (y - g) % 7 == 3 && y < g + tower_h - 6) m = B::Glass;
                    if (y == g + tower_h) m = B::StoneBricks;
                    bp.set(x, y, z, m);
                }
        bp.set(tx0 + 3, g + tower_h - 2, tz0 + 3, B::GoldBlock);  // campana
        bp.set(tx0 + 3, g + tower_h - 1, tz0 + 3, B::Fence);
        for (int k = 0; k <= 4; ++k) {
            const int y = g + tower_h + 1 + k * 2;
            for (int z = tz0 - 1; z <= tz1 + 1; ++z)
                for (int x = tx0 - 1; x <= tx1 + 1; ++x) {
                    const int dd = std::max(std::abs(x - (tx0 + 3)), std::abs(z - (tz0 + 3)));
                    if (dd == 4 - k) {
                        bp.set(x, y, z, ctx.roof);
                        bp.set(x, y + 1, z, ctx.roof);
                    } else if (dd < 4 - k) {
                        bp.set(x, y, z, B::Air);
                        bp.set(x, y + 1, z, B::Air);
                    }
                }
        }
        bp.set(tx0 + 3, g + tower_h + 11, tz0 + 3, B::GoldBlock);
        // Puerta del campanario desde la nave lateral.
        bp.set(tx0 + 3, g + 1, tz1, B::Air);
        bp.set(tx0 + 3, g + 2, tz1, B::Air);
    };
    belfry(0);
    belfry(W - 7);
    // Interior: pasillo central con alfombra, bancos, aranas, braseros y el altar en el abside.
    const int altar_z = bz1 - 4;
    for (int z = bz0 + 1; z < altar_z - 1; ++z) bp.set(cx, g, z, B::RugRed);
    for (int z = bz0 + 3; z < altar_z - 4; z += 2)
        for (int x = px0 + 1; x < px1; ++x)
            if (std::abs(x - cx) > 1) bp.set(x, g + 1, z, B::Bench);
    for (int z = bz0 + 4; z < bz1 - 2; z += 6) bp.set(cx, g + nave_h - 3, z, B::Chandelier);
    for (int z = bz0 + 2; z < bz1; z += 4) {
        bp.set(ax0 + 1, g + 1, z, B::Brazier);
        bp.set(ax1 - 1, g + 1, z, B::Brazier);
    }
    for (int z = altar_z - 2; z < bz1; ++z)
        for (int x = px0 + 1; x < px1; ++x) bp.set(x, g + 1, z, B::CarvedStone);  // estrado
    for (int x = cx - 1; x <= cx + 1; ++x) bp.set(x, g + 2, altar_z, B::Altar);
    bp.set(cx - 1, g + 3, altar_z, B::Candles);
    bp.set(cx + 1, g + 3, altar_z, B::Candles);
    bp.set(cx, g + 3, altar_z, B::GoldBlock);
    for (int y = g + 2; y <= g + 8; ++y) {
        bp.set(cx - 3, y, bz1, B::BannerBlue);
        bp.set(cx + 3, y, bz1, B::BannerBlue);
    }
    // Organo (estanterias) en la nave lateral derecha, al fondo.
    for (int y = g + 1; y <= g + 4; ++y) bp.set(ax1 - 1, y, bz1 - 2, B::Bookshelf);
    ctx.point("altar", static_cast<float>(cx), static_cast<float>(g + 3), static_cast<float>(altar_z), 0);
    ctx.point("npc", static_cast<float>(cx), static_cast<float>(g + 2), static_cast<float>(altar_z - 1), 0, "sacerdote");
    ctx.point("npc", static_cast<float>(cx - 3), static_cast<float>(g + 1), static_cast<float>(bz0 + 6), 2, "monje");
    ctx.point("npc", static_cast<float>(cx + 3), static_cast<float>(g + 1), static_cast<float>(bz0 + 10), 2, "monja");
    ctx.point("mercader", static_cast<float>(ax0 + 2), static_cast<float>(g + 1), static_cast<float>(bz1 - 3), 1, "pociones");
    // --- Cripta (y 0..g): bajo las tres naves, con nichos y ataudes ---
    stoneRoom(ctx, ax0, 0, bz0 + 1, ax1, g, bz1 - 3, B::DungeonBricks);
    for (int z = bz0 + 1; z <= bz1 - 3; ++z)
        for (int x = ax0; x <= ax1; ++x) bp.set(x, g, z, (x + z) % 2 ? B::Paving : B::Marble);  // el suelo de arriba
    for (int z = bz0 + 4; z <= bz1 - 5; z += 4)
        for (int y = 1; y < g; ++y) {
            bp.set(px0, y, z, B::CarvedStone);
            bp.set(px1, y, z, B::CarvedStone);
        }
    for (int z = bz0 + 3; z <= bz1 - 5; z += 3) {
        bp.set(ax0 + 1, 1, z, B::Coffin);
        bp.set(ax1 - 1, 1, z, B::Coffin);
        bp.set(ax0 + 1, 2, z + 1, B::Candles);
        if (ctx.rand(z, 41) < 0.4f) bp.set(ax1 - 1, 1, z + 1, B::Bones);
        if (ctx.rand(z, 42) < 0.3f) bp.set(cx + (z % 2 ? 2 : -2), 3, z, B::Cobweb);
    }
    for (int z = bz0 + 5; z <= bz1 - 5; z += 6) bp.set(cx, 3, z, B::Lantern);
    // Escalera de bajada (nave lateral izquierda, hacia el fondo) con barandilla arriba.
    const int sx = ax0 + 2, sz = bz0 + 8;
    for (int j = 1; j <= g; ++j) {
        const int z = sz + j;
        bp.set(sx, g - j, z, B::StoneBricks);
        for (int y = g - j + 1; y <= g - j + 3; ++y) bp.set(sx, y, z, B::Air);
    }
    for (int z = sz + 1; z <= sz + 3; ++z) bp.set(sx + 1, g + 1, z, B::Fence);
    // Osario sellado: tras un muro falso al fondo de la cripta, bajo el abside.
    const int oz0 = bz1 - 3;  // muro del fondo de la cripta
    stoneRoom(ctx, cx - 3, 0, oz0, cx + 3, 4, bz1 + 1, B::DungeonBricks);
    bp.set(cx, 1, oz0, B::SecretWall);
    bp.set(cx, 2, oz0, B::SecretWall);
    for (int x = cx - 2; x <= cx + 2; ++x) bp.set(x, 1, bz1, x == cx ? B::Air : B::SkullPile);
    bp.set(cx - 2, 1, oz0 + 1, B::Bones);
    bp.set(cx + 2, 2, oz0 + 1, B::Candles);
    lootChest(ctx, cx, 1, bz1, 0, 3, "catacumbas");
    loreNote(ctx, cx + 2, 1, oz0 + 1, 3, "catacumbas");
}

// --- Cementerio con mausoleo ----------------------------------------------------------------

void graveyard(BuildContext& ctx, int w, int d) {
    constexpr int g = 6;
    w = std::max(w, 13);
    d = std::max(d, 13);
    ctx.bp = Blueprint(w, g + 9, d);
    Blueprint& bp = ctx.bp;
    const int c = w / 2;
    for (int z = 0; z < d; ++z)
        for (int x = 0; x < w; ++x) {
            const bool ring = x == 0 || z == 0 || x == w - 1 || z == d - 1;
            if (ring) {
                bp.set(x, g, z, B::Cobblestone);
                bp.set(x, g + 1, z, (z == 0 && std::abs(x - c) <= 1) ? B::Air : ((x + z) % 4 == 0 ? B::StoneBricks : B::Fence));
                continue;
            }
            if (std::abs(x - c) <= 0) bp.set(x, g, z, B::Gravel);
            bp.set(x, g + 1, z, B::Air);
        }
    // Lapidas en filas (y alguna tumba con flores).
    for (int z = 2; z < d - 8; z += 2)
        for (int x = 2; x < w - 2; x += 2) {
            if (std::abs(x - c) <= 1) continue;
            if (ctx.rand(x, z, 3) < 0.8f) bp.set(x, g + 1, z, B::Gravestone);
            if (ctx.rand(x, z, 4) < 0.2f) bp.set(x, g + 1, z + 1, ctx.rand(x, z, 5) < 0.5f ? B::WhiteFlower : B::Candles);
        }
    // Arbol seco en una esquina.
    for (int y = g + 1; y <= g + 4; ++y) bp.set(1 + (w > 15 ? 1 : 0), y, d - 2, B::SpruceLog);
    bp.set(2 + (w > 15 ? 1 : 0), g + 4, d - 2, B::Branch);
    // Mausoleo (7 x 7) al fondo, con escalera a la cripta.
    const int mx0 = c - 3, mz0 = d - 8, mx1 = c + 3, mz1 = d - 2;
    for (int y = g; y <= g + 5; ++y)
        for (int z = mz0; z <= mz1; ++z)
            for (int x = mx0; x <= mx1; ++x) {
                const bool wall = x == mx0 || x == mx1 || z == mz0 || z == mz1;
                if (y == g) bp.set(x, y, z, B::CarvedStone);
                else if (y == g + 5) bp.set(x, y, z, B::CarvedStone);
                else bp.set(x, y, z, wall ? ((x == mx0 || x == mx1) && z == mz0 ? B::MarblePillar : B::Marble) : B::Air);
            }
    for (int x = mx0 + 1; x < mx1; ++x) bp.set(x, g + 6, (mz0 + mz1) / 2, B::CarvedStone);
    bp.set(c, g + 7, (mz0 + mz1) / 2, B::Gravestone);
    bp.set(c, g + 1, mz0, B::Door);
    bp.set(c, g + 2, mz0, B::Door);
    bp.set(c - 1, g + 3, mz0 - 1, B::Lantern);
    // Cripta debajo: escalera desde el mausoleo hacia el fondo.
    stoneRoom(ctx, mx0 - 1, 0, mz0 - 5, mx1 + 1, g, mz1, B::DungeonBricks);
    for (int z = mz0 - 4; z <= mz1 - 1; ++z)
        for (int x = mx0; x <= mx1; ++x) bp.set(x, g, z, z >= mz0 ? B::CarvedStone : B::Stone);
    for (int j = 1; j <= g; ++j) {
        const int z = mz0 + j - 1;
        if (z >= mz1) break;
        bp.set(c, g - j, z, B::StoneBricks);
        for (int y = g - j + 1; y <= g - j + 3; ++y) bp.set(c, y, z, B::Air);
    }
    for (int z = mz0 - 3; z < mz1; z += 2) {
        bp.set(mx0, 1, z, B::Coffin);
        bp.set(mx1, 1, z, B::Coffin);
    }
    bp.set(mx0, 2, mz0 - 2, B::Candles);
    bp.set(mx1, 2, mz0 - 2, B::Candles);
    bp.set(c, 3, mz0 - 2, B::Lantern);
    lootChest(ctx, c, 1, mz0 - 4, 2, 2, "cripta");
    loreNote(ctx, c + 2, 1, mz0 - 4, 2, "cripta");
    ctx.point("npc", static_cast<float>(c), static_cast<float>(g + 1), 2, 2, "sepulturero");
}

// --- Sede de gremio ---------------------------------------------------------------------------

void guildHall(BuildContext& ctx, const std::string& guild) {
    const int variant = static_cast<int>(ctx.rand(1) * 1000.0f);
    Palette p = paletteFor(ctx, variant);
    if (ctx.style != BuildStyle::Desert) {
        p.wall_low = B::StoneBricks;
        p.found = B::CarvedStone;
    }
    Reserve* rp = nullptr;
    std::unique_ptr<Reserve> holder;
    const int w = 13, d = 11;
    const Shell s = houseShell(ctx, rp, holder, w, d, 3, p, false);
    Reserve& r = *rp;
    Blueprint& bp = ctx.bp;
    const BlockId banner = guild == "herreros" ? B::BannerRed : (guild == "magos" ? B::BannerPurple : (guild == "ladrones" ? B::BannerSkull : B::BannerBlue));
    // Camara secreta (piso 1, esquina del fondo derecha) tras una estanteria falsa.
    const int sy = 5;
    const int rx0 = s.ix1 - 2, rz0 = s.iz1 - 2;
    for (int y = sy; y <= sy + 2; ++y) {
        for (int z = rz0 - 1; z <= s.iz1; ++z) {
            bp.set(rx0 - 1, y, z, B::Bookshelf);
            r.mark(rx0 - 1, y, z);
            r.mark(rx0 - 2, y, z);
        }
        for (int x = rx0 - 1; x <= s.ix1; ++x) {
            bp.set(x, y, rz0 - 1, B::Bookshelf);
            r.mark(x, y, rz0 - 1);
        }
        for (int z = rz0; z <= s.iz1; ++z)
            for (int x = rx0; x <= s.ix1; ++x) r.mark(x, y, z);
    }
    bp.set(rx0 - 1, sy, rz0, B::SecretBookshelf);
    bp.set(rx0 - 1, sy + 1, rz0, B::SecretBookshelf);
    lootChest(ctx, s.ix1, sy, s.iz1, 3, 2, "gremio");
    loreNote(ctx, rx0, sy, s.iz1, 0, "gremio:" + guild);
    bp.set(s.ix1, sy, rz0, B::Candles);
    bp.set(rx0 + 1, sy + 2, rz0 + 1, B::Lantern);
    // Planta baja: mesas largas, mostrador y estandartes.
    for (int x = s.ix0 + 3; x <= s.ix1 - 1; ++x) {
        if (place(bp, r, x, 1, (s.iz0 + s.iz1) / 2, B::Table)) {
            if (x % 2) bp.set(x, 2, (s.iz0 + s.iz1) / 2, B::Candles);
            place(bp, r, x, 1, (s.iz0 + s.iz1) / 2 - 1, B::Bench);
            place(bp, r, x, 1, (s.iz0 + s.iz1) / 2 + 1, B::Bench);
        }
    }
    for (int x = s.ix0 + 2; x <= s.ix1; ++x) place(bp, r, x, 1, s.iz1, x % 2 ? B::Shelf : B::Crate);
    for (int y = 1; y <= 3; ++y) {
        bp.set(s.door_x - 2, y, s.z0 - 1, y == 3 ? banner : kKeep);
        bp.set(s.door_x + 2, y + 1, s.z0, banner);
        bp.set(s.door_x - 2, y + 1, s.z0, banner);
    }
    place(bp, r, (s.ix0 + s.ix1) / 2, 3, (s.iz0 + s.iz1) / 2, B::Chandelier);
    ctx.point("npc", static_cast<float>(s.ix1 - 1), 1, static_cast<float>(s.iz0 + 1), 0, "maestro_gremio");
    ctx.point("mercader", static_cast<float>((s.ix0 + s.ix1) / 2), 1, static_cast<float>(s.iz1 - 1), 0,
              guild == "herreros" ? "armas" : (guild == "magos" ? "pociones" : "general"));
    // Piso 1: archivo (estanterias y escritorio).
    for (int x = s.ix0 + 2; x < rx0 - 2; ++x)
        if (place(bp, r, x, sy, s.iz1, B::Bookshelf)) place(bp, r, x, sy + 1, s.iz1, B::Bookshelf);
    if (place(bp, r, s.ix0 + 3, sy, s.iz0 + 2, B::Table)) {
        bp.set(s.ix0 + 3, sy + 1, s.iz0 + 2, B::Candles);
        place(bp, r, s.ix0 + 3, sy, s.iz0 + 1, B::Chair);
    }
    place(bp, r, s.ix0 + 4, sy + 2, s.iz0 + 3, B::Lantern);
    // Piso 2: dormitorio del maestre.
    furnishBedroom(ctx, s, r, 2, p, variant, false);
    // Escudo dorado sobre la puerta.
    bp.set(s.door_x, 5, s.z0 - 1, B::GoldBlock);
}

// --- Casona noble con jardin cerrado --------------------------------------------------------

void manor(BuildContext& ctx) {
    constexpr int W = 17, D = 21;
    const int variant = static_cast<int>(ctx.rand(1) * 1000.0f);
    // La casa (11 x 9 por dentro, 3 pisos) en un plano aparte.
    BuildContext hc = ctx;
    hc.points.clear();
    Palette p = paletteFor(hc, variant);
    if (hc.style != BuildStyle::Desert) p.wall_low = B::StoneBricks;
    Reserve* rp = nullptr;
    std::unique_ptr<Reserve> holder;
    const Shell s = houseShell(hc, rp, holder, 11, 9, 3, p, true);
    Reserve& r = *rp;
    // Estudio secreto (piso 1, fondo izquierda) tras una estanteria falsa.
    const int sy = 5;
    for (int y = sy; y <= sy + 2; ++y) {
        for (int x = s.ix0; x <= s.ix0 + 3; ++x) {
            hc.bp.set(x, y, s.iz1 - 3, B::Bookshelf);
            r.mark(x, y, s.iz1 - 3);
            r.mark(x, y, s.iz1 - 4);
        }
        for (int z = s.iz1 - 2; z <= s.iz1; ++z) {
            hc.bp.set(s.ix0 + 3, y, z, B::Bookshelf);
            r.mark(s.ix0 + 3, y, z);
            for (int x = s.ix0; x <= s.ix0 + 2; ++x) r.mark(x, y, z);
        }
    }
    hc.bp.set(s.ix0 + 3, sy, s.iz1 - 1, B::SecretBookshelf);
    hc.bp.set(s.ix0 + 3, sy + 1, s.iz1 - 1, B::SecretBookshelf);
    lootChest(hc, s.ix0, sy, s.iz1, 1, 2, "noble");
    loreNote(hc, s.ix0 + 1, sy, s.iz1, 0, "noble");
    hc.bp.set(s.ix0, sy, s.iz1 - 2, B::Table);
    hc.bp.set(s.ix0, sy + 1, s.iz1 - 2, B::Candles);
    const RoomSpots living = furnishLiving(hc, s, r, 0, p, variant);
    hc.point("npc", living.npc_x, 1, living.npc_z, 2, "noble");
    furnishBedroom(hc, s, r, 1, p, variant + 1, false);
    const RoomSpots bed = furnishBedroom(hc, s, r, 2, p, variant + 2, false);
    if (bed.bed) hc.point("cama", static_cast<float>(bed.bed_x), 10, static_cast<float>(bed.bed_z), 0);
    hc.point("npc", bed.npc_x, 9, bed.npc_z, 0, "dama");
    // Componer: casa al fondo (x 2..14, z 10..20), jardin delante.
    ctx.bp = Blueprint(W, hc.bp.h, D);
    Blueprint& bp = ctx.bp;
    blit(bp, hc.bp, 2, 0, 10);
    for (const LocalPoint& lp : hc.points) ctx.points.push_back(LocalPoint{lp.kind, lp.x + 2.0f, lp.y, lp.z + 10.0f, lp.dir, lp.tag});
    const int c = W / 2;
    // Tapia (2 de alto) con verja delante.
    for (int z = 0; z < D; ++z)
        for (int x = 0; x < W; ++x) {
            const bool ring = x == 0 || z == 0 || x == W - 1 || z == D - 1;
            if (!ring) continue;
            if (bp.get(x, 1, z) != kKeep && bp.get(x, 1, z) != B::Air) continue;
            bp.set(x, 0, z, B::StoneBricks);
            const bool gate = z == 0 && std::abs(x - c) <= 1;
            bp.set(x, 1, z, gate ? B::Air : (ctx.style == BuildStyle::Desert ? B::MudBricks : B::StoneBricks));
            bp.set(x, 2, z, gate ? B::Air : ((x + z) % 3 == 0 ? B::Lantern : B::Fence));
        }
    // Jardin: camino de grava, setos, fuente pequena y dos arboles.
    for (int z = 1; z < 10; ++z)
        for (int x = 1; x < W - 1; ++x) {
            bp.set(x, 0, z, std::abs(x - c) <= 1 ? B::Gravel : B::Grass);
            bp.set(x, 1, z, B::Air);
            bp.set(x, 2, z, B::Air);
            const bool hedge = (x == 2 || x == W - 3) && z > 1 && z < 9;
            if (hedge) bp.set(x, 1, z, B::Bush);
            else if ((x == c - 3 || x == c + 3) && z % 3 == 1) bp.set(x, 1, z, z % 2 ? B::RedFlower : B::BlueFlower);
        }
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) bp.set(c + dx - 4, 1, 5 + dz, (dx == 0 && dz == 0) ? B::PoolWater : B::Marble);
    bp.set(c - 4, 2, 5, B::MarblePillar);
    for (int i = 0; i < 2; ++i) {
        const int tx = i ? W - 4 : 3, tz = 8;
        for (int y = 1; y <= 4; ++y) bp.set(tx, y, tz, B::OakLog);
        for (int y = 4; y <= 6; ++y)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    if (bp.get(tx + dx, y, tz + dz) == B::Air || bp.get(tx + dx, y, tz + dz) == kKeep) bp.set(tx + dx, y, tz + dz, B::OakLeaves);
    }
    bp.set(c + 4, 1, 4, B::Bench);
    bp.set(c + 5, 1, 4, B::Bench);
    ctx.point("npc", static_cast<float>(c + 4), 1, 5, 2, "jardinero");
}

// --- Taller -------------------------------------------------------------------------------------

void workshop(BuildContext& ctx, const std::string& kind) {
    const int variant = static_cast<int>(ctx.rand(1) * 1000.0f);
    Palette p = paletteFor(ctx, variant);
    Reserve* rp = nullptr;
    std::unique_ptr<Reserve> holder;
    const Shell s = houseShell(ctx, rp, holder, 10, 8, 2, p, kind == "alfarero");
    Reserve& r = *rp;
    Blueprint& bp = ctx.bp;
    // Mostrador abierto a la calle (dos huecos junto a la puerta; el resto, muro).
    for (int x = s.door_x + 1; x < std::min(s.x1, s.door_x + 3); ++x)
        if (bp.get(x, 2, s.z0) != p.beam) {
            bp.set(x, 2, s.z0, B::Air);
            bp.set(x, 3, s.z0, B::Air);
        }
    const int y = 1;
    if (kind == "curtidor") {
        for (int z = s.iz0 + 2; z <= s.iz0 + 3; ++z)
            for (int x = s.ix0 + 1; x <= s.ix0 + 2; ++x) bp.set(x, 0, z, B::PoolWater);
        for (int x = s.ix0 + 4; x <= s.ix1; x += 2) {
            place(bp, r, x, y, s.iz1, B::Fence);
            place(bp, r, x, y + 1, s.iz1, B::HideTent);
        }
        place(bp, r, s.ix1, y, s.iz0 + 2, B::Barrel);
    } else if (kind == "carpintero") {
        for (int x = s.ix0 + 1; x <= s.ix0 + 3; ++x) {
            place(bp, r, x, y, s.iz1, B::OakLog);
            place(bp, r, x, y + 1, s.iz1, B::OakPlanks);
        }
        place(bp, r, s.ix1 - 1, y, s.iz1 - 1, B::CraftingTable);
        place(bp, r, s.ix1, y, s.iz1 - 1, B::Crate);
        place(bp, r, s.ix0 + 1, y, s.iz0 + 3, B::Table);
    } else if (kind == "tejedor") {
        static constexpr BlockId kCloth[3] = {B::ClothRed, B::ClothBlue, B::ClothYellow};
        for (int x = s.ix0 + 1, i = 0; x <= s.ix1; x += 2, ++i) {
            place(bp, r, x, y, s.iz1, kCloth[i % 3]);
            place(bp, r, x, y + 1, s.iz1, B::ClothWhite);
        }
        place(bp, r, s.ix0 + 2, y, s.iz0 + 3, B::Table);
        place(bp, r, s.ix0 + 2, y + 1, s.iz0 + 3, B::Fence);
    } else {  // alfarero
        for (int x = s.ix0 + 1; x <= s.ix1 - 2; x += 2) {
            place(bp, r, x, y, s.iz1, B::Clay);
            place(bp, r, x + 1, y, s.iz1, B::Terracotta);
        }
        place(bp, r, s.ix0 + 1, y, s.iz0 + 3, B::Oven);
    }
    place(bp, r, (s.ix0 + s.ix1) / 2, 3, (s.iz0 + s.iz1) / 2, B::Lantern);
    for (int x = s.door_x + 1; x <= std::min(s.door_x + 2, s.ix1); ++x) place(bp, r, x, y, s.iz0, B::Counter);
    furnishBedroom(ctx, s, r, 1, p, variant, false);
    ctx.point("npc", static_cast<float>(s.ix0 + 2), 1, static_cast<float>(s.iz0 + 2), 2, kind);
    ctx.point("mercader", static_cast<float>(s.door_x + 1), 1, static_cast<float>(s.iz0 + 1), 0, "general");
}

// --- Cuartel y patio de armas -----------------------------------------------------------------

void barracks(BuildContext& ctx) {
    Palette p = paletteFor(ctx, 2);
    if (ctx.style != BuildStyle::Desert) {
        p.wall_low = p.wall = B::StoneBricks;
        p.beam = p.ridge = B::DarkPlanks;
        p.found = B::Cobblestone;
    }
    p.roof = B::RoofSlate;
    Reserve* rp = nullptr;
    std::unique_ptr<Reserve> holder;
    const Shell s = houseShell(ctx, rp, holder, 15, 9, 2, p, true);
    Reserve& r = *rp;
    Blueprint& bp = ctx.bp;
    for (int z = s.iz0 + 1; z <= s.iz1; z += 2) {
        if (place(bp, r, s.ix0, 1, z, B::WeaponRack)) place(bp, r, s.ix0, 2, z, B::WeaponRack);
    }
    for (int x = s.ix0 + 3; x <= s.ix1 - 3; ++x) {
        if (place(bp, r, x, 1, (s.iz0 + s.iz1) / 2, B::Table)) {
            if (x % 3 == 0) bp.set(x, 2, (s.iz0 + s.iz1) / 2, B::Candles);
            place(bp, r, x, 1, (s.iz0 + s.iz1) / 2 + 1, B::Bench);
        }
    }
    for (int x = s.ix0 + 2; x <= s.ix1 - 1; x += 2) {
        if (place(bp, r, x, 5, s.iz1, B::Bed)) {
            place(bp, r, x, 5, s.iz1 - 1, B::Bed);
            place(bp, r, x, 5, s.iz1 - 2, B::Chest);
        }
        if (place(bp, r, x, 5, s.iz0, B::Bed)) place(bp, r, x, 5, s.iz0 + 1, B::Bed);
    }
    for (int x = s.ix0 + 2; x <= s.ix1 - 2; x += 4) place(bp, r, x, 7, (s.iz0 + s.iz1) / 2, B::Lantern);
    place(bp, r, (s.ix0 + s.ix1) / 2, 3, s.iz0 + 1, B::Lantern);
    bp.set(s.door_x - 2, 2, s.z0, B::BannerRed);
    bp.set(s.door_x + 2, 2, s.z0, B::BannerRed);
    ctx.point("npc", static_cast<float>(s.ix1 - 1), 1, static_cast<float>(s.iz0 + 1), 0, "capitan");
    for (int i = 0; i < 3; ++i) ctx.point("npc", static_cast<float>(s.ix0 + 3 + i * 3), 1, static_cast<float>(s.iz1 - 1), 0, "soldado");
    ctx.point("mercader", static_cast<float>(s.ix0 + 1), 1, static_cast<float>(s.iz0 + 1), 1, "armaduras");
}

void trainingYard(BuildContext& ctx, int w, int d) {
    ctx.bp = Blueprint(w, 4, d);
    Blueprint& bp = ctx.bp;
    const int c = w / 2;
    for (int z = 0; z < d; ++z)
        for (int x = 0; x < w; ++x) {
            const bool ring = x == 0 || z == 0 || x == w - 1 || z == d - 1;
            bp.set(x, 0, z, ctx.rand(x, z, 7) < 0.3f ? B::Gravel : B::DirtPath);
            bp.set(x, 1, z, ring && !(z == 0 && std::abs(x - c) <= 1) ? B::Fence : B::Air);
            bp.set(x, 2, z, B::Air);
        }
    // Munecos de practica y dianas.
    for (int x = 2; x < w - 2; x += 3) {
        bp.set(x, 1, 2, B::Fence);
        bp.set(x, 2, 2, B::HayBale);
    }
    for (int x = 2; x < w - 2; x += 4) {
        bp.set(x, 1, d - 2, B::HayBale);
        bp.set(x, 2, d - 2, B::ClothRed);
    }
    bp.set(1, 1, d / 2, B::WeaponRack);
    bp.set(w - 2, 1, d / 2, B::WeaponRack);
    bp.set(w - 2, 1, d / 2 + 1, B::Barrel);
    ctx.point("npc", static_cast<float>(c - 2), 1, static_cast<float>(d / 2), 0, "soldado");
    ctx.point("npc", static_cast<float>(c + 2), 1, static_cast<float>(d / 2), 0, "soldado");
}

// --- Almacen ------------------------------------------------------------------------------------

void warehouse(BuildContext& ctx) {
    Palette p = paletteFor(ctx, 5);
    p.wall_low = p.wall = ctx.style == BuildStyle::Desert ? B::MudBricks : B::DarkPlanks;
    p.window = B::Air;
    Reserve* rp = nullptr;
    std::unique_ptr<Reserve> holder;
    const Shell s = houseShell(ctx, rp, holder, 11, 9, 1, p, false);
    Reserve& r = *rp;
    Blueprint& bp = ctx.bp;
    for (int x = s.door_x - 1; x <= s.door_x + 1; ++x)
        for (int y = 1; y <= 3; ++y) bp.set(x, y, s.z0, B::Air);
    for (int z = s.iz0 + 1; z <= s.iz1; ++z)
        for (int x = s.ix0; x <= s.ix1; ++x) {
            if (std::abs(x - s.door_x) <= 1) continue;
            const float q = ctx.rand(x, z, 31);
            if (q < 0.6f && place(bp, r, x, 1, z, q < 0.3f ? B::Crate : B::Barrel) && q < 0.35f) place(bp, r, x, 2, z, B::Crate);
        }
    place(bp, r, s.door_x, 3, (s.iz0 + s.iz1) / 2, B::Lantern);
    ctx.point("npc", static_cast<float>(s.door_x), 1, static_cast<float>(s.iz0 + 1), 0, "estibador");
}

// --- Banco con camara acorazada -------------------------------------------------------------

void bank(BuildContext& ctx) {
    constexpr int g = 6;
    const int variant = static_cast<int>(ctx.rand(1) * 1000.0f);
    BuildContext hc = ctx;
    hc.points.clear();
    Palette p = paletteFor(hc, variant);
    if (hc.style != BuildStyle::Desert) {
        p.wall_low = p.wall = B::StoneBricks;
        p.found = B::CarvedStone;
        p.beam = B::CarvedStone;
    }
    Reserve* rp = nullptr;
    std::unique_ptr<Reserve> holder;
    const Shell s = houseShell(hc, rp, holder, 11, 10, 2, p, false);
    Reserve& r = *rp;
    // Bajada a la camara (muro izquierdo, hacia el fondo): la franja queda libre.
    for (int z = s.iz0 + 2; z <= s.iz0 + 8 && z <= s.iz1; ++z) r.markColumn(s.ix0, 1, 3, z);
    const int cz = s.iz0 + 2;
    for (int x = s.ix0 + 2; x <= s.ix1; ++x) place(hc.bp, r, x, 1, cz, B::Counter);
    for (int x = s.ix0 + 2; x <= s.ix1; x += 2) hc.bp.set(x, 2, cz, B::Candles);
    for (int x = s.ix0 + 2; x <= s.ix1; ++x) place(hc.bp, r, x, 1, s.iz1, x % 2 ? B::GoldBlock : B::Chest);
    place(hc.bp, r, (s.ix0 + s.ix1) / 2, 3, s.iz0 + 1, B::Chandelier);
    hc.point("mercader", static_cast<float>((s.ix0 + s.ix1) / 2), 1, static_cast<float>(cz + 1), 0, "banquero");
    hc.point("npc", static_cast<float>(s.ix1 - 1), 1, static_cast<float>(s.iz0 + 1), 0, "guardia");
    furnishBedroom(hc, s, r, 1, p, variant, false);
    hc.bp.set(s.door_x - 1, 3, s.z0 - 1, B::GoldBlock);
    hc.bp.set(s.door_x + 1, 3, s.z0 - 1, B::GoldBlock);
    // Componer con la camara debajo.
    ctx.bp = Blueprint(hc.bp.w, hc.bp.h + g, hc.bp.d);
    Blueprint& bp = ctx.bp;
    blit(bp, hc.bp, 0, g, 0);
    for (const LocalPoint& lp : hc.points) ctx.points.push_back(LocalPoint{lp.kind, lp.x, lp.y + static_cast<float>(g), lp.z, lp.dir, lp.tag});
    // Camara: antesala (delante) y caja fuerte (fondo) tras rejas, con palancas.
    stoneRoom(ctx, s.x0, 0, s.z0, s.x1, g, s.z1, B::StoneBricks);
    for (int z = s.z0; z <= s.z1; ++z)
        for (int x = s.x0; x <= s.x1; ++x) bp.set(x, g, z, (x == s.x0 || x == s.x1 || z == s.z0 || z == s.z1) ? p.found : p.floor);
    for (int j = 1; j <= g; ++j) {
        const int z = s.iz0 + 1 + j;
        bp.set(s.ix0, g - j, z, B::StoneBricks);
        for (int y = g - j + 1; y <= g - j + 3; ++y) bp.set(s.ix0, y, z, B::Air);
    }
    const int vz = s.iz1 - 2;  // reja
    for (int x = s.ix0 + 1; x <= s.ix1; ++x)
        for (int y = 1; y <= 3; ++y) bp.set(x, y, vz, B::IronBars);
    for (int z = vz + 1; z <= s.iz1; ++z)
        for (int x = s.ix0 + 1; x <= s.ix1; ++x) bp.set(x, 0, z, B::GoldBlock);
    lootChest(ctx, s.ix1, 1, s.iz1, 3, 3, "banco");
    lootChest(ctx, s.ix0 + 2, 1, s.iz1, 0, 3, "banco");
    bp.set((s.ix0 + s.ix1) / 2, 1, s.iz1, B::GoldBlock);
    bp.set((s.ix0 + s.ix1) / 2, 3, vz + 1, B::Lantern);
    bp.set(s.ix0 + 2, 3, s.iz0 + 1, B::Lantern);
    // Puzle: tres palancas en el muro de delante; la combinacion abre la reja (el juego).
    const int pattern = 1 + static_cast<int>(ctx.rand(9) * 6.99f);  // 1..7 (bits de las palancas)
    for (int i = 0; i < 3; ++i) {
        const int x = s.ix0 + 2 + i * 2;
        bp.set(x, 1, s.iz0, B::RuneLever);
        ctx.point("puzle", static_cast<float>(x), 1, static_cast<float>(s.iz0), 0,
                  "banco:" + std::to_string(i) + ":" + std::to_string((pattern >> i) & 1));
    }
    for (int x = s.ix0 + 1; x <= s.ix1; ++x) ctx.point("puzle_puerta", static_cast<float>(x), 1, static_cast<float>(vz), 0, "banco");
    loreNote(ctx, s.ix1, 1, s.iz0 + 1, 3, "puzle:banco:" + std::to_string(pattern));
    // Y un muro falso en el costado de la caja fuerte (para quien lo encuentre).
    bp.set(s.x1, 1, vz + 1, B::SecretWall);
    bp.set(s.x1, 2, vz + 1, B::SecretWall);
}

// --- Huerto y molino -----------------------------------------------------------------------------

void orchard(BuildContext& ctx, int w, int d) {
    ctx.bp = Blueprint(w, 8, d);
    Blueprint& bp = ctx.bp;
    for (int z = 0; z < d; ++z)
        for (int x = 0; x < w; ++x) {
            const bool ring = x == 0 || z == 0 || x == w - 1 || z == d - 1;
            bp.set(x, 1, z, ring && !(z == 0 && x == w / 2) ? B::Fence : B::Air);
        }
    for (int z = 2; z < d - 2; z += 4)
        for (int x = 2; x < w - 2; x += 4) {
            const int h = 2 + static_cast<int>(ctx.rand(x, z) * 2.0f);
            for (int y = 1; y <= h; ++y) bp.set(x, y, z, B::OakLog);
            for (int y = h; y <= h + 2; ++y)
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (std::abs(dx) + std::abs(dz) + (y - h) > 2 && ctx.rand(x + dx, y, z + dz) < 0.5f) continue;
                        if (bp.get(x + dx, y, z + dz) == B::Air || bp.get(x + dx, y, z + dz) == kKeep)
                            bp.set(x + dx, y, z + dz, ctx.rand(x + dx, y + 7, z + dz) < 0.1f ? B::RedFlower : B::OakLeaves);
                    }
        }
    bp.set(1, 1, d - 2, B::Barrel);
    bp.set(2, 1, d - 2, B::Crate);
    ctx.point("npc", static_cast<float>(w / 2), 1, 1, 2, "granjero");
}

void windmill(BuildContext& ctx) {
    constexpr int S = 9, H = 14;
    ctx.bp = Blueprint(S, H + 6, S);
    Blueprint& bp = ctx.bp;
    const int c = S / 2;
    for (int y = 0; y <= H; ++y)
        for (int z = 2; z <= 6; ++z)
            for (int x = 2; x <= 6; ++x) {
                const bool ring = x == 2 || x == 6 || z == 2 || z == 6;
                BlockId m = y == 0 ? B::Cobblestone : (ring ? (y <= 3 ? B::Cobblestone : ((x == 2 || x == 6) && (z == 2 || z == 6) ? B::Timber : B::Plaster)) : B::Air);
                if (ring && y == 7 && (x == c || z == c)) m = B::Glass;
                bp.set(x, y, z, m);
            }
    for (int k = 0; k <= 3; ++k)
        for (int z = 1 + k; z <= 7 - k; ++z)
            for (int x = 1 + k; x <= 7 - k; ++x) {
                const bool ring = x == 1 + k || x == 7 - k || z == 1 + k || z == 7 - k;
                bp.set(x, H + 1 + k, z, ring ? B::Thatch : B::Air);
            }
    // Aspas en la cara de delante (z 1), cruz de 4 brazos.
    const int hy = H - 2;
    bp.set(c, hy, 1, B::DarkPlanks);
    for (int i = 1; i <= 5; ++i) {
        bp.set(c + i, hy, 1, i % 2 ? B::ClothWhite : B::OakPlanks);
        bp.set(c - i, hy, 1, i % 2 ? B::ClothWhite : B::OakPlanks);
        bp.set(c, hy + i, 1, i % 2 ? B::ClothWhite : B::OakPlanks);
        if (hy - i > 3) bp.set(c, hy - i, 1, i % 2 ? B::ClothWhite : B::OakPlanks);
    }
    bp.set(c, 1, 2, B::Door);
    bp.set(c, 2, 2, B::Door);
    bp.set(c, 1, c, B::Stone);
    bp.set(c + 1, 1, c + 1, B::HayBale);
    bp.set(c - 1, 1, c + 1, B::HayBale);
    bp.set(c, 3, c, B::Lantern);
    ctx.point("npc", static_cast<float>(c), 1, 1, 2, "molinero");
}

// --- Subterraneo: pozo seco, salas y tuneles ----------------------------------------------------

void dryWell(BuildContext& ctx, int depth) {
    // 5 x 5: brocal arriba (y = depth) y escalera de caracol hasta abajo, con salida a los 4 lados.
    ctx.bp = Blueprint(5, depth + 4, 5);
    Blueprint& bp = ctx.bp;
    static constexpr int kRing[8][2] = {{1, 1}, {2, 1}, {3, 1}, {3, 2}, {3, 3}, {2, 3}, {1, 3}, {1, 2}};
    for (int y = 0; y <= depth; ++y)
        for (int z = 0; z < 5; ++z)
            for (int x = 0; x < 5; ++x) {
                const bool ring = x == 0 || z == 0 || x == 4 || z == 4;
                if (ring) bp.set(x, y, z, y == 0 ? B::Cobblestone : oldStone(ctx, x, y, z));
                else if (x == 2 && z == 2) bp.set(x, y, z, B::Cobblestone);
                else bp.set(x, y, z, y == 0 ? B::Cobblestone : B::Air);
            }
    for (int y = 1; y < depth; ++y) {
        const int i = ((depth - y) % 8 + 8) % 8;
        bp.set(kRing[i][0], y, kRing[i][1], B::StoneBricks);
    }
    // Brocal (1 por encima del suelo) con tejadillo.
    for (int z = 0; z < 5; ++z)
        for (int x = 0; x < 5; ++x) {
            const bool ring = x == 0 || z == 0 || x == 4 || z == 4;
            bp.set(x, depth + 1, z, ring ? B::Cobblestone : B::Air);
            bp.set(x, depth + 2, z, ((x == 0 || x == 4) && (z == 0 || z == 4)) ? B::Timber : B::Air);
            bp.set(x, depth + 3, z, ctx.roof);
        }
    bp.set(2, depth + 1, 0, B::Air);  // entrada
    bp.set(2, depth + 2, 2, B::Lantern);
    bp.set(2, depth + 1, 2, B::Air);
    bp.set(2, depth, 2, B::Air);
    for (int y = 4; y < depth - 2; y += 5) bp.set(2, y, 2, B::Glowstone);
    // Salidas abajo (los tuneles llegan por cualquier lado).
    for (int y = 1; y <= 2; ++y) {
        bp.set(2, y, 0, B::Air);
        bp.set(2, y, 4, B::Air);
        bp.set(0, y, 2, B::Air);
        bp.set(4, y, 2, B::Air);
    }
    ctx.point("entrada", 2, static_cast<float>(depth + 1), -1, 2, "tunel");
}

void tunnel(BuildContext& ctx, int length) {
    ctx.bp = Blueprint(5, 5, length);
    Blueprint& bp = ctx.bp;
    for (int z = 0; z < length; ++z)
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 5; ++x) {
                const bool shell = x == 0 || x == 4 || y == 0 || y == 4;
                BlockId m = B::Air;
                if (shell) m = y == 0 ? (ctx.rand(x, z, 3) < 0.3f ? B::Gravel : B::Cobblestone) : (ctx.rand(x, y, z) < 0.5f ? B::Stone : oldStone(ctx, x, y, z));
                bp.set(x, y, z, m);
            }
    for (int z = 2; z < length - 1; z += 6) {  // entibado
        for (int y = 1; y <= 3; ++y) {
            bp.set(1, y, z, B::OakLog);
            bp.set(3, y, z, B::OakLog);
        }
        for (int x = 1; x <= 3; ++x) bp.set(x, 3, z, B::DarkPlanks);
        if ((z / 6) % 2 == 0) bp.set(2, 2, z, B::Lantern);
    }
    for (int z = 1; z < length - 1; ++z)
        if (ctx.rand(z, 51) < 0.12f) bp.set(ctx.rand(z, 52) < 0.5f ? 1 : 3, 3, z, B::Cobweb);
}

void undergroundRoom(BuildContext& ctx, int iw, int id, const std::string& kind, int tier) {
    const int W = iw + 2, D = id + 2;
    ctx.bp = Blueprint(W, 6, D);
    Blueprint& bp = ctx.bp;
    stoneRoom(ctx, 0, 0, 0, W - 1, 5, D - 1, B::Cobblestone);
    const int c = W / 2;
    for (int y = 1; y <= 3; ++y)
        for (int x = c - 1; x <= c + 1; ++x) {
            bp.set(x, y, 0, B::Air);
            bp.set(x, y, D - 1, B::Air);
        }
    for (int x = 2; x < W - 2; x += 4) bp.set(x, 3, D / 2, B::Lantern);
    if (kind == "guarida") {
        for (int z = 2; z < D - 2; z += 2) {
            bp.set(1, 1, z, B::HayBale);
            bp.set(W - 2, 1, z, z % 4 == 0 ? B::Crate : B::Barrel);
        }
        bp.set(c + 2, 1, D / 2, B::Table);
        bp.set(c + 2, 2, D / 2, B::Candles);
        bp.set(c + 3, 1, D / 2, B::Chair);
        bp.set(c - 2, 1, D / 2 - 1, B::Campfire);
        lootChest(ctx, W - 2, 1, D / 2, 3, tier, "ladrones");
        loreNote(ctx, 1, 1, D / 2 + 1, 1, "ladrones");
        ctx.point("npc", static_cast<float>(c + 1), 1, static_cast<float>(D / 2 + 1), 0, "ladron");
    } else if (kind == "puzle") {
        // Tres palancas en el muro izquierdo; la reja del nicho de la derecha guarda el cofre.
        const int pattern = 1 + static_cast<int>(ctx.rand(13) * 6.99f);
        for (int i = 0; i < 3; ++i) {
            const int z = 2 + i * 2;
            if (z >= D - 1) break;
            bp.set(1, 1, z, B::RuneLever);
            bp.set(1, 3, z, B::SignalLamp);
            ctx.point("puzle", 1, 1, static_cast<float>(z), 1, "tunel:" + std::to_string(i) + ":" + std::to_string((pattern >> i) & 1));
        }
        const int nz = D / 2;
        for (int z = nz - 1; z <= nz + 1; ++z)
            for (int y = 1; y <= 3; ++y) bp.set(W - 2, y, z, B::IronBars);
        // Nicho detras de la reja (la sala crece hacia +x por fuera del plano: se hace dentro).
        lootChest(ctx, W - 3, 1, nz, 1, tier, "puzle");
        bp.set(W - 3, 1, nz - 1, B::GoldBlock);
        bp.set(W - 3, 1, nz + 1, B::Candles);
        for (int z = nz - 1; z <= nz + 1; ++z)
            for (int y = 1; y <= 3; ++y) bp.set(W - 4, y, z, B::IronBars);
        for (int z = nz - 1; z <= nz + 1; ++z) ctx.point("puzle_puerta", static_cast<float>(W - 4), 1, static_cast<float>(z), 3, "tunel");
        loreNote(ctx, c, 1, D - 2, 0, "puzle:tunel:" + std::to_string(pattern));
        bp.set(c, 1, 1, B::RuneStone);
    } else if (kind == "boveda") {
        for (int z = 1; z < D - 1; ++z)
            for (int x = 1; x < W - 1; ++x)
                if (ctx.rand(x, z, 61) < 0.25f && std::abs(x - c) > 1) bp.set(x, 1, z, B::GoldBlock);
        lootChest(ctx, 1, 1, 1, 1, tier, "tesoro_real");
        lootChest(ctx, W - 2, 1, D - 2, 3, tier, "tesoro_real");
        loreNote(ctx, W - 2, 1, 1, 3, "castillo");
    } else {  // cripta
        for (int z = 2; z < D - 2; z += 2) {
            bp.set(1, 1, z, B::Coffin);
            bp.set(W - 2, 1, z, B::Coffin);
        }
        bp.set(c, 1, D / 2, B::Altar);
        bp.set(c, 2, D / 2, B::Candles);
        lootChest(ctx, c, 1, D - 2, 0, tier, "cripta");
    }
}

// --- Santuario misterioso ------------------------------------------------------------------------

void shrine(BuildContext& ctx, int variant) {
    constexpr int S = 17, g = 6;
    ctx.bp = Blueprint(S, g + 14, S);
    Blueprint& bp = ctx.bp;
    const int c = S / 2;
    // Plataforma de piedra vieja.
    for (int z = 0; z < S; ++z)
        for (int x = 0; x < S; ++x) {
            const int dx = x - c, dz = z - c;
            const int d2 = dx * dx + dz * dz;
            if (d2 <= 25) bp.set(x, g, z, oldStone(ctx, x, g, z));
            else if (d2 <= 64 && ctx.rand(x, z, 71) < 0.4f) bp.set(x, g, z, B::Gravel);
            if (d2 <= 64)
                for (int y = g + 1; y <= g + 6; ++y) bp.set(x, y, z, B::Air);
        }
    if (variant == 0) {
        // Circulo de menhires.
        static constexpr int kStones[8][2] = {{7, 0}, {5, 5}, {0, 7}, {-5, 5}, {-7, 0}, {-5, -5}, {0, -7}, {5, -5}};
        for (int i = 0; i < 8; ++i) {
            const int h = 3 + static_cast<int>(ctx.rand(i, 81) * 3.0f);
            for (int y = g + 1; y <= g + h; ++y) bp.set(c + kStones[i][0], y, c + kStones[i][1], y == g + h ? B::CarvedStone : oldStone(ctx, i, y, 0));
            if (i % 2 == 0 && h >= 4) bp.set(c + kStones[i][0], g + 2, c + kStones[i][1], B::RuneStone);
        }
    } else if (variant == 1) {
        // Capilla en ruinas: muros rotos sin tejado.
        for (int z = c - 5; z <= c + 5; ++z)
            for (int x = c - 4; x <= c + 4; ++x) {
                const bool wall = x == c - 4 || x == c + 4 || z == c - 5 || z == c + 5;
                if (!wall || (z == c - 5 && std::abs(x - c) <= 1)) continue;
                const int h = 2 + static_cast<int>(ctx.rand(x, z, 91) * 5.0f);
                for (int y = g + 1; y <= g + h; ++y) bp.set(x, y, z, oldStone(ctx, x, y, z));
                if (ctx.rand(x, z, 92) < 0.2f) bp.set(x, g + h + 1, z, B::Bush);
            }
        bp.set(c - 3, g + 1, c + 3, B::Bones);
        bp.set(c + 3, g + 1, c - 2, B::Cobweb);
    } else {
        // Obelisco de runas con braseros de fuego de alma.
        for (int y = g + 1; y <= g + 11; ++y) {
            bp.set(c, y, c + 3, y % 3 == 0 ? B::GlyphStone : B::CarvedStone);
            if (y <= g + 3) {
                bp.set(c - 1, y, c + 3, B::CarvedStone);
                bp.set(c + 1, y, c + 3, B::CarvedStone);
            }
        }
        bp.set(c - 4, g + 1, c - 1, B::SoulBrazier);
        bp.set(c + 4, g + 1, c - 1, B::SoulBrazier);
    }
    // Altar en el centro: una losa falsa delante baja a la cripta (se cae al pisarla).
    bp.set(c, g + 1, c, B::Altar);
    bp.set(c - 1, g + 1, c, B::Candles);
    bp.set(c + 1, g + 1, c, B::Candles);
    loreNote(ctx, c, g + 1, c + 1, 0, "santuario:" + std::to_string(variant));
    // Cripta: escalera bajo una losa falsa (SecretWall) detras del altar.
    stoneRoom(ctx, c - 4, 0, c - 6, c + 4, 5, c - 1, B::DungeonBricks);
    // Escalera recta hacia -z desde (c, g, c - 1) hasta el suelo de la cripta.
    for (int j = 0; j < g; ++j) {
        const int z = c - 1 - j;
        bp.set(c, g - 1 - j, z, B::StoneBricks);
        for (int y = g - j; y <= g - j + 2; ++y) bp.set(c, y, z, B::Air);
    }
    bp.set(c, g, c - 1, B::SecretWall);  // la losa falsa
    bp.set(c + 2, 1, c - 5, B::SoulBrazier);
    bp.set(c - 2, 1, c - 5, B::SoulBrazier);
    bp.set(c - 3, 1, c - 3, B::Bones);
    lootChest(ctx, c, 1, c - 5, 2, 2 + (variant == 2 ? 1 : 0), "santuario");
    ctx.point("altar", static_cast<float>(c), static_cast<float>(g + 2), static_cast<float>(c), 2);
}
