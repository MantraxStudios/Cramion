// CIUDADES-GRANDES: metropolis medievales, castillos en colinas y santuarios. Se incluye DENTRO
// de struct Planner (Structures.cpp): usa sus ayudas (site, begin, add, hillField, groundRoad...).
//
// Metropolis: una (o ninguna) por celda de 3 x 3 regiones (1152 m; la celda 0,0 = las regiones
// del origen no tiene nunca). Se planifica una vez por celda (Structures::metropolis) y las
// regiones que toca la comparten (RegionPlan::shared): asi puede medir 300-500 m sin caber en una
// region. Trazado de tela de arana: plaza del mercado en el centro, avenidas desde 4 puertas,
// rondas concentricas y radiales con un recodo cada tramo (organico), callejones entre medias,
// muralla redonda de 16 lados con torres, castillo en lo alto, catedral con su plaza, barrios
// (mercaderes, nobles, artesanos, pobres, puerto), campos y huertos fuera. Todo con enteros y
// tablas fijas de direcciones (nada de sin/cos/atan2: igual en todas las plataformas).

// 16 direcciones (como ARBOLES-HZ: vectores fijos).
static constexpr float kD16[16][2] = {{1.0f, 0.0f},        {0.9239f, 0.3827f},   {0.7071f, 0.7071f},   {0.3827f, 0.9239f},
                                      {0.0f, 1.0f},        {-0.3827f, 0.9239f},  {-0.7071f, 0.7071f},  {-0.9239f, 0.3827f},
                                      {-1.0f, 0.0f},       {-0.9239f, -0.3827f}, {-0.7071f, -0.7071f}, {-0.3827f, -0.9239f},
                                      {0.0f, -1.0f},       {0.3827f, -0.9239f},  {0.7071f, -0.7071f},  {0.9239f, -0.3827f}};
static constexpr float kMetroChance = 0.6f;  // celdas de 3 x 3 regiones con metropolis

// Ocupacion por celda para los trazados grandes (rapida: sin listas de rectangulos).
struct Occ {
    enum : std::uint8_t { Free = 0, Street = 1, Building = 2, Square = 3, Rampart = 4, Reserved = 5 };
    int x0 = 0, z0 = 0, n = 0;
    std::vector<std::uint8_t> c;
    void init(int X0, int Z0, int N) {
        x0 = X0;
        z0 = Z0;
        n = N;
        c.assign(static_cast<std::size_t>(N) * static_cast<std::size_t>(N), Free);
    }
    std::uint8_t at(int x, int z) const {
        const int i = x - x0, j = z - z0;
        if (i < 0 || j < 0 || i >= n || j >= n) return 255;
        return c[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * static_cast<std::size_t>(n)];
    }
    void set(int x, int z, std::uint8_t v) {
        const int i = x - x0, j = z - z0;
        if (i < 0 || j < 0 || i >= n || j >= n) return;
        c[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * static_cast<std::size_t>(n)] = v;
    }
    bool rectFree(const Rect& r, int margin) const {
        for (int z = r.z0; z <= r.z1; ++z)
            for (int x = r.x0; x <= r.x1; ++x)
                if (at(x, z) != Free) return false;
        if (margin <= 0) return true;
        for (int z = r.z0 - margin; z <= r.z1 + margin; ++z)
            for (int x = r.x0 - margin; x <= r.x1 + margin; ++x) {
                if (x >= r.x0 && x <= r.x1 && z >= r.z0 && z <= r.z1) continue;
                const std::uint8_t v = at(x, z);
                if (v == Building || v == Rampart || v == Reserved) return false;
            }
        return true;
    }
    void fill(const Rect& r, std::uint8_t v) {
        for (int z = r.z0; z <= r.z1; ++z)
            for (int x = r.x0; x <= r.x1; ++x) set(x, z, v);
    }
    // Celdas a menos de `reach` del segmento (centros de celda).
    void segment(float ax, float az, float bx, float bz, float reach, std::uint8_t v, bool only_free) {
        const float vx = bx - ax, vz = bz - az, len2 = vx * vx + vz * vz;
        const int x0_ = static_cast<int>(std::floor(std::min(ax, bx) - reach)), x1_ = static_cast<int>(std::ceil(std::max(ax, bx) + reach));
        const int z0_ = static_cast<int>(std::floor(std::min(az, bz) - reach)), z1_ = static_cast<int>(std::ceil(std::max(az, bz) + reach));
        for (int z = z0_; z <= z1_; ++z)
            for (int x = x0_; x <= x1_; ++x) {
                const float px = static_cast<float>(x) + 0.5f, pz = static_cast<float>(z) + 0.5f;
                float u = len2 > 0.0f ? ((px - ax) * vx + (pz - az) * vz) / len2 : 0.0f;
                u = std::clamp(u, 0.0f, 1.0f);
                const float dx = px - (ax + vx * u), dz = pz - (az + vz * u);
                if (dx * dx + dz * dz > reach * reach) continue;
                if (only_free && at(x, z) != Free) continue;
                set(x, z, v);
            }
    }
    void disc(float cx, float cz, float r, std::uint8_t v) { segment(cx, cz, cx, cz, r, v, false); }
};

static int nearestDir16(float fx, float fz, int step) {
    int best = 0;
    float bd = -9.0f;
    for (int k = 0; k < 16; k += step) {
        const float d = kD16[k][0] * fx + kD16[k][1] * fz;
        if (d > bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

// Perfil (altura por metro) de un tramo entre dos alturas conocidas: sigue el terreno suavizado
// subiendo o bajando como mucho 1 cada 2 m y llega a hb (si no se puede, la rampa justa).
std::vector<int> profileBetween(const Hill& hf, float ax, float az, float bx, float bz, int ha, int hb) const {
    const float len = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
    const int steps = std::max(1, static_cast<int>(std::ceil(len)));
    std::vector<int> p(static_cast<std::size_t>(steps + 1));
    int h16 = ha * 16;
    for (int t = 0; t <= steps; ++t) {
        const float u = static_cast<float>(t) / static_cast<float>(steps);
        const int x = static_cast<int>(std::floor(ax + (bx - ax) * u)), z = static_cast<int>(std::floor(az + (bz - az) * u));
        if (t > 0) h16 += std::clamp(hf.smooth16(x, z) - h16, -8, 8);
        const int lo = hb * 16 - (steps - t) * 8, hi = hb * 16 + (steps - t) * 8;
        h16 = std::clamp(h16, lo, hi);
        p[static_cast<std::size_t>(t)] = std::max(roundDiv(h16, 16), sea + 1);
    }
    p.front() = ha;
    p.back() = hb;
    return p;
}

int hillHeight(const Hill& hf, float x, float z) const {
    return std::max(roundDiv(hf.smooth16(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(z))), 16), sea + 1);
}

// Calle de A a B (alturas ha, hb) por un recodo M (altura del terreno): suelo, ocupacion y Road.
void metroStreet(Structure& s, Occ& occ, const Hill& hf, float ax, float az, int ha, float bx, float bz, int hb, float half, BlockId surf,
                 BlockId alt, float bend, std::uint32_t salt) {
    const float vx = bx - ax, vz = bz - az;
    const float len = std::sqrt(vx * vx + vz * vz);
    if (len < 1.0f) return;
    const float j = (static_cast<float>(hashOf(seed, static_cast<int>(salt), static_cast<int>(ax * 3.0f), static_cast<int>(az * 3.0f)) & 0xFFFF) / 65536.0f - 0.5f) * 2.0f;
    const float mx = ax + vx * 0.5f - vz / len * bend * j, mz = az + vz * 0.5f + vx / len * bend * j;
    const int lo = std::max(ha, hb) - static_cast<int>(len * 0.25f), hi = std::min(ha, hb) + static_cast<int>(len * 0.25f);
    const int hm = lo <= hi ? std::clamp(hillHeight(hf, mx, mz), lo, hi) : (ha + hb) / 2;
    const float pts[3][2] = {{ax, az}, {mx, mz}, {bx, bz}};
    const int hs[3] = {ha, std::max(hm, sea + 1), hb};
    for (int i = 0; i < 2; ++i) {
        const Road r{pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], half, surf, alt, 0.12f};
        groundRoad(s, r, profileBetween(hf, r.x0, r.z0, r.x1, r.z1, hs[i], hs[i + 1]));
        occ.segment(r.x0, r.z0, r.x1, r.z1, half + 0.5f, Occ::Street, true);
        s.roads.push_back(r);
    }
}

// Plataforma de un edificio: el rectangulo y su borde (sin pisar calles, plazas ni otras casas).
static void metroPad(Structure& s, const Occ& occ, const Rect& r, int h) {
    for (int z = r.z0 - 1; z <= r.z1 + 1; ++z)
        for (int x = r.x0 - 1; x <= r.x1 + 1; ++x) {
            const bool in = x >= r.x0 && x <= r.x1 && z >= r.z0 && z <= r.z1;
            int gh = 0;
            std::uint8_t kind = 0;
            if (!s.groundAt(x, z, gh, &kind)) continue;
            if (!in && (kind == Structure::GRoad || kind == Structure::GStep || kind == Structure::GPlaza || occ.at(x, z) != Occ::Free)) continue;
            groundPut(s, x, z, h, Structure::GPad);
        }
}

// Disco de plaza (suelo llano a h).
static void metroSquare(Structure& s, Occ& occ, float cx, float cz, float r, int h) {
    const int x0 = static_cast<int>(std::floor(cx - r)), x1 = static_cast<int>(std::ceil(cx + r));
    const int z0 = static_cast<int>(std::floor(cz - r)), z1 = static_cast<int>(std::ceil(cz + r));
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx, dz = static_cast<float>(z) + 0.5f - cz;
            if (dx * dx + dz * dz > r * r) continue;
            groundPut(s, x, z, h, Structure::GPlaza);
            occ.set(x, z, Occ::Square);
        }
}

// Pieza en un rectangulo que mira hacia (fx, fz), centrada en (ccx, ccz). false si no cabe.
template <typename Fn>
bool metroPlace(Structure& s, Occ& occ, const Hill& hf, BuildContext& ctx, Fn&& fn, int w, int d, float ccx, float ccz, float fx, float fz, int floor_y,
                int yoff, int margin, int max_cut) {
    const int rot = rotFacing(fx, fz);
    const int sx = (rot & 1) ? d : w, sz = (rot & 1) ? w : d;
    const int x0 = static_cast<int>(std::lround(ccx - static_cast<float>(sx) * 0.5f));
    const int z0 = static_cast<int>(std::lround(ccz - static_cast<float>(sz) * 0.5f));
    const Rect r{x0, z0, x0 + sx - 1, z0 + sz - 1};
    if (!occ.rectFree(r, margin)) return false;
    // Ni zanjas ni terraplenes enormes (el terreno bajo las esquinas y el centro).
    const int probes[5][2] = {{r.x0, r.z0}, {r.x1, r.z0}, {r.x0, r.z1}, {r.x1, r.z1}, {(r.x0 + r.x1) / 2, (r.z0 + r.z1) / 2}};
    for (const auto& p : probes)
        if (std::abs(hillHeight(hf, static_cast<float>(p[0]), static_cast<float>(p[1])) - floor_y) > max_cut) return false;
    occ.fill(r, Occ::Building);
    fn(ctx);
    add(s, ctx, x0, floor_y + yoff, z0, rot);
    metroPad(s, occ, r, floor_y);
    return true;
}

enum MetroDistrict : int { DMarket = 0, DNoble, DArtisan, DPoor, DResidential, DHarbor, DTemple, DOutside };
enum MetroKind : int {
    MTown = 0, MSmall, MTavern, MSmithy, MShopGeneral, MShopPotions, MShopArmor, MStable, MChapel, MGuildMerchants, MGuildSmiths,
    MGuildMages, MGuildThieves, MBank, MManor, MBarracks, MYard, MWarehouse, MWorkTanner, MWorkCarpenter, MWorkWeaver, MWorkPotter,
    MGarden, MFarmhouse, MGraveyard
};

std::string metroName(Rng& r, bool capital) {
    static const char* const kCap[] = {"Aurelhold", "Highmarch", "Kingsbridge", "Silverspire", "Ambermoor", "Thornecrown", "Valewatch",
                                       "Goldenreach", "Stormhaven", "Crownmere", "Brightwall", "Oldcastle"};
    static const char* const kCapForm[] = {"Royal City of ", "", "Grand ", "", "High City of "};
    r.next();
    if (capital) return std::string(r.pick(kCapForm)) + r.pick(kCap);
    static const char* const kA[] = {"Amber", "Raven", "Stone", "Gold", "Iron", "Silver", "Thorn", "Wolf", "Oak", "Salt", "Copper", "Lantern",
                                     "Bell", "Swan", "Hawk", "Ash", "Elder", "Rose", "Storm", "Hollow", "Briar", "Kings"};
    static const char* const kB[] = {"ford", "haven", "bridge", "gate", "wick", "moor", "hold", "mere", "burgh", "watch", "port", "march"};
    static const char* const kForm[] = {"", "Free City of ", "", "Old ", "", ""};
    return std::string(r.pick(kForm)) + r.pick(kA) + r.pick(kB);
}

// --- La metropolis de la celda (mi, mj) --------------------------------------------------------
bool buildMetropolis(int mi, int mj) {
    if (dimension != "superficie" || (mi == 0 && mj == 0)) return false;
    Rng mr(hashOf(seed, mi, mj, 0x3E7A));
    if (mr.f() >= kMetroChance) return false;
    const bool capital = mr.f() < 0.3f;
    const int R = capital ? 210 + mr.range(0, 35) : 140 + mr.range(0, 35);
    const int mcx = 3 * mi * kRegionSize, mcz = 3 * mj * kRegionSize;
    const int lim = 3 * kRegionSize / 2 - R - 105;
    if (lim < 8) return false;
    // Sitio: el mejor de 5 x 5 candidatos (seco, poco desnivel, sin montana ni paramo).
    int cx = 0, cz = 0;
    float best = -1e30f;
    SiteStats bst;
    for (int j = 0; j < 5; ++j)
        for (int i = 0; i < 5; ++i) {
            // (dentro de la celda: las regiones de otra celda no la ven)
            const int x = mcx + std::clamp((i - 2) * lim / 2 + mr.range(-lim / 8, lim / 8), -lim, lim);
            const int z = mcz + std::clamp((j - 2) * lim / 2 + mr.range(-lim / 8, lim / 8), -lim, lim);
            const SiteStats st = site(x, z, R, 9);
            if (st.water > 2 || st.max - st.min > 38 || st.min < sea + 1) continue;
            if (st.biome == Generator::AMountains || st.biome == Generator::AAshlands || st.biome == Generator::ABlight || st.biome == Generator::AOcean)
                continue;
            const float score = -static_cast<float>(st.max - st.min) * 2.0f - static_cast<float>(st.water) * 25.0f -
                                static_cast<float>(std::abs(x - mcx) + std::abs(z - mcz)) * 0.01f + mr.f() * 4.0f;
            if (score > best) {
                best = score;
                cx = x;
                cz = z;
                bst = st;
            }
        }
    if (best < -1e29f) return false;
    BuildStyle bstyle = styleFor(bst.biome);
    if (bstyle == BuildStyle::Beach) bstyle = BuildStyle::Temperate;
    const int E = R + 96;
    Hill hf = hillField(cx, cz, E, 8);
    const int base = std::max(roundDiv(hf.smooth16(cx, cz), 16), sea + 2);
    hf.base = base;
    Rng nr(hashOf(seed, mi, mj, 0x3E7B));
    Structure& s = begin("ciudad", metroName(nr, capital), cx, base + 1, cz, levelAt(cx, cz, capital ? 1.0f : 0.5f));
    s.info.id = ((static_cast<std::uint64_t>(hashOf(seed, mi, mj, 0x3E7C)) << 20) | 0xFFFFFu) & ((std::uint64_t{1} << 52) - 1);
    s.info.seed = hashOf(seed, mi, mj, 0x3E7D);
    s.clear_radius = static_cast<float>(R + 96);
    s.seal = true;
    Terraform tf;
    tf.cx = cx;
    tf.cz = cz;
    tf.inner = static_cast<float>(R + 78);
    tf.outer = static_cast<float>(R + 96);
    tf.base = base;
    tf.surface = surfaceFor(bstyle);
    tf.under = bstyle == BuildStyle::Desert ? B::Sand : B::Dirt;
    tf.decor = 0.05f;
    s.terraforms.push_back(tf);
    // Suelo: bancales del terreno en toda la ventana.
    s.ground_x0 = hf.x0;
    s.ground_z0 = hf.z0;
    s.ground_n = hf.n;
    s.ground.assign(static_cast<std::size_t>(hf.n) * static_cast<std::size_t>(hf.n), 0);
    s.ground_kind.assign(static_cast<std::size_t>(hf.n) * static_cast<std::size_t>(hf.n), Structure::GTerrain);
    for (int j = 0; j < hf.n; ++j)
        for (int i = 0; i < hf.n; ++i) groundPut(s, hf.x0 + i, hf.z0 + j, terraceAt(hf, hf.x0 + i, hf.z0 + j), Structure::GTerrain);
    Occ occ;
    occ.init(hf.x0, hf.z0, hf.n);
    const float fcx = static_cast<float>(cx) + 0.5f, fcz = static_cast<float>(cz) + 0.5f;
    const float fR = static_cast<float>(R);
    const auto jit = [&](int a, int b, int c2) { return static_cast<float>(hashOf(seed, a + mi * 131, b + mj * 137, c2) & 0xFFFF) / 65536.0f - 0.5f; };

    // --- Radiales, rondas y nodos ---
    const float P = std::round(fR * (capital ? 0.15f : 0.16f));
    // Rondas: manzanas de ~25-30 m (dos filas de casas espalda con espalda y un patio).
    const int NR = capital ? 6 : 5;  // bandas de ~30-35 m entre la plaza y la ronda de la muralla
    float ringR[6] = {};
    for (int i = 0; i < NR; ++i) ringR[i] = P + 1.0f + (fR - 17.0f - P) * static_cast<float>(i) / static_cast<float>(NR - 1);
    // Radiales: 8 cerca del centro y 16 en la mitad de fuera.
    const auto active = [&](int k, int ring) { return (k % 2 == 0) || ringR[ring] >= fR * 0.5f; };
    struct Node {
        float x = 0, z = 0;
        int h = 0;
        bool ok = false;
    };
    Node node[16][6];
    for (int k = 0; k < 16; ++k)
        for (int ring = 0; ring < NR; ++ring) {
            if (!active(k, ring)) continue;
            const bool gate = k % 4 == 0;
            const float r = ringR[ring] * (ring == 0 || ring == NR - 1 ? 1.0f : 1.0f + jit(k, ring, 1) * 0.07f);
            const float pj = (ring == 0 || (gate && ring == NR - 1)) ? 0.0f : jit(k, ring, 2) * fR * (k % 2 ? 0.06f : 0.045f);
            Node& n = node[k][ring];
            n.x = fcx + kD16[k][0] * r - kD16[k][1] * pj;
            n.z = fcz + kD16[k][1] * r + kD16[k][0] * pj;
            n.h = ring == 0 ? base : hillHeight(hf, n.x, n.z);
            n.ok = true;
        }
    // Castillo: en el radial diagonal mas alto; catedral enfrente.
    int kc = 2;
    {
        int bh = -1 << 30;
        for (int k = 2; k < 16; k += 4) {
            const int h = hf.smooth16(static_cast<int>(fcx + kD16[k][0] * fR * 0.55f), static_cast<int>(fcz + kD16[k][1] * fR * 0.55f));
            if (h > bh) {
                bh = h;
                kc = k;
            }
        }
    }
    const int kt = (kc + 8) % 16;
    const int castle_w = capital ? 51 : 47, castle_d = capital ? 44 : 40;
    const float castleR = fR * 0.56f;
    const float Cx = fcx + kD16[kc][0] * castleR, Cz = fcz + kD16[kc][1] * castleR;
    const float rc = static_cast<float>(castle_w) * 0.72f + 4.0f;  // disco que ocupa (girado)
    const int castle_h = std::max(hillHeight(hf, Cx, Cz), base + 2);
    const int castle_rot = rotFacing(fcx - Cx, fcz - Cz);  // la puerta mira a la plaza
    // Catedral: plaza en el nodo de la primera ronda y la iglesia detras (hacia fuera).
    const Node& tq = node[kt][1];
    const float Tq = 11.0f;  // radio de la plaza
    constexpr int kCathW = 23, kCathD = 45;
    // La catedral sigue el eje mas cercano al radial (las piezas solo giran de 90 en 90).
    const int cath_rot = rotFacing(-kD16[kt][0], -kD16[kt][1]);
    const float tox = -static_cast<float>(kFront[cath_rot][0]), toz = -static_cast<float>(kFront[cath_rot][1]);  // hacia fuera
    const float Tcx = tq.x + tox * (Tq + 2.0f + kCathD * 0.5f), Tcz = tq.z + toz * (Tq + 2.0f + kCathD * 0.5f);
    const auto inCastle = [&](float x, float z) { return (x - Cx) * (x - Cx) + (z - Cz) * (z - Cz) < (rc + 3.0f) * (rc + 3.0f); };
    const auto inCathedral = [&](float x, float z) {
        // Rectangulo girado (eje kt): coordenadas a lo largo y a lo ancho.
        const float dx = x - Tcx, dz = z - Tcz;
        const float along = dx * tox + dz * toz, across = -dx * toz + dz * tox;
        return std::abs(along) < kCathD * 0.5f + 3.0f && std::abs(across) < kCathW * 0.5f + 3.0f;
    };
    const auto blocked = [&](float ax, float az, float bx, float bz) {
        const float len = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
        const int n = std::max(2, static_cast<int>(len / 2.0f));
        for (int i = 0; i <= n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n);
            const float x = ax + (bx - ax) * u, z = az + (bz - az) * u;
            if (inCastle(x, z) || inCathedral(x, z)) return true;
        }
        return false;
    };
    // Plazas: mercado, la de la catedral y plazuelas en las avenidas (segunda ronda).
    metroSquare(s, occ, fcx, fcz, P, base);
    metroSquare(s, occ, tq.x, tq.z, Tq, tq.h);
    for (int k = 0; k < 16; k += 4) metroSquare(s, occ, node[k][2].x, node[k][2].z, 7.0f, node[k][2].h);
    // Reservas: castillo y catedral (las calles no entran).
    occ.disc(Cx, Cz, rc, Occ::Reserved);
    {
        const float hl = kCathD * 0.5f + 1.0f, hw = kCathW * 0.5f + 1.0f;
        for (float a = -hl; a <= hl; a += 0.5f)
            for (float b = -hw; b <= hw; b += 0.5f)
                occ.set(static_cast<int>(std::floor(Tcx + tox * a - toz * b)), static_cast<int>(std::floor(Tcz + toz * a + tox * b)), Occ::Reserved);
    }
    // --- Muralla redonda de 16 lados (puertas en los 4 ejes) ---
    float wallR[16];
    for (int k = 0; k < 16; ++k) wallR[k] = k % 4 == 0 ? fR : fR * (1.0f + jit(k, 9, 3) * 0.08f);
    const auto wallPt = [&](int k, float& x, float& z) {
        x = fcx + kD16[k][0] * wallR[k];
        z = fcz + kD16[k][1] * wallR[k];
    };
    constexpr int kWallH = 11;
    for (int k = 0; k < 16; ++k) {
        float ax, az, bx, bz;
        wallPt(k, ax, az);
        wallPt((k + 1) % 16, bx, bz);
        const float vx = bx - ax, vz = bz - az, len = std::sqrt(vx * vx + vz * vz);
        // Recorte junto a las puertas (el cuerpo de la puerta ocupa 7.5 m a cada lado).
        if (k % 4 == 0) {
            ax += vx / len * 7.5f;
            az += vz / len * 7.5f;
        }
        if ((k + 1) % 4 == 0) {
            bx -= vx / len * 7.5f;
            bz -= vz / len * 7.5f;
        }
        s.walls.push_back(WallLine{ax, az, bx, bz, kWallH, 2.5f});
        occ.segment(ax, az, bx, bz, 4.0f, Occ::Rampart, false);
    }
    // --- Calles ---
    const BlockId pave = bstyle == BuildStyle::Desert ? B::Sandstone : B::Paving;
    const BlockId cobble = bstyle == BuildStyle::Desert ? B::SandstoneBricks : B::Cobblestone;
    const BlockId lane = bstyle == BuildStyle::Desert ? B::Sand : B::Gravel;
    struct Edge {
        float ax, az, bx, bz;
        int ha, hb;
        float half;
        int kind;  // 0 avenida, 1 ronda, 2 radial, 3 callejon, 4 fuera
    };
    std::vector<Edge> edges;
    const auto edge = [&](const Node& a, const Node& b, float half, int kind) {
        if (!a.ok || !b.ok) return;
        if (blocked(a.x, a.z, b.x, b.z)) return;
        edges.push_back(Edge{a.x, a.z, b.x, b.z, a.h, b.h, half, kind});
    };
    // Rondas (con su punto medio como nodo para los callejones).
    std::vector<std::array<Node, 2>> ring_mid[6];
    for (int ring = 1; ring < NR; ++ring) {
        int prev = -1, first = -1;
        for (int k = 0; k <= 16; ++k) {
            const int kk = k % 16;
            if (!active(kk, ring)) continue;
            if (prev >= 0 && !(k == 16 && first < 0)) {
                const Node& a = node[prev][ring];
                const Node& b = node[kk][ring];
                Node m;
                const float ex = b.x - a.x, ez = b.z - a.z, el = std::sqrt(ex * ex + ez * ez);
                const float bj = jit(prev, ring, 5) * el * 0.18f;
                m.x = (a.x + b.x) * 0.5f - ez / std::max(el, 1.0f) * bj;
                m.z = (a.z + b.z) * 0.5f + ex / std::max(el, 1.0f) * bj;
                m.h = hillHeight(hf, m.x, m.z);
                m.ok = true;
                const float half = ring == NR - 1 ? 2.2f : (ring % 2 ? 2.2f : 1.9f);
                const std::size_t before = edges.size();
                edge(a, m, half, 1);
                edge(m, b, half, 1);
                if (edges.size() == before + 2) ring_mid[ring].push_back({m, Node{static_cast<float>(prev), static_cast<float>(kk), 0, true}});
            }
            if (first < 0) first = kk;
            prev = kk;
            if (k == 16) break;
        }
    }
    // Radiales: avenidas (puertas) y calles.
    float gateX[4], gateZ[4], outX[4], outZ[4];
    int gateH[4], outH[4];
    for (int k = 0; k < 16; ++k) {
        for (int ring = 0; ring + 1 < NR; ++ring) {
            if (!node[k][ring].ok || !node[k][ring + 1].ok) continue;
            const bool gate = k % 4 == 0;
            edge(node[k][ring], node[k][ring + 1], gate ? 3.0f : 1.7f, gate ? 0 : 2);
        }
        if (k % 4 == 0) {
            const int g = k / 4;
            gateX[g] = fcx + kD16[k][0] * fR;
            gateZ[g] = fcz + kD16[k][1] * fR;
            gateH[g] = hillHeight(hf, gateX[g], gateZ[g]);
            outX[g] = fcx + kD16[k][0] * (fR + 80.0f);
            outZ[g] = fcz + kD16[k][1] * (fR + 80.0f);
            outH[g] = hillHeight(hf, outX[g], outZ[g]);
            const Node& r3 = node[k][NR - 1];
            edges.push_back(Edge{r3.x, r3.z, gateX[g], gateZ[g], r3.h, gateH[g], 3.0f, 0});
            edges.push_back(Edge{gateX[g], gateZ[g], outX[g], outZ[g], gateH[g], outH[g], 2.6f, 4});
        }
    }
    // Avenida del castillo (desde la plaza) y de la catedral ya llega por su radial.
    const float cgx = Cx + static_cast<float>(kFront[castle_rot][0]) * (static_cast<float>(castle_d) * 0.5f + 1.0f);
    const float cgz = Cz + static_cast<float>(kFront[castle_rot][1]) * (static_cast<float>(castle_d) * 0.5f + 1.0f);
    edges.insert(edges.begin(), Edge{node[kc][0].x, node[kc][0].z, cgx, cgz, base, castle_h, 2.6f, 0});
    // Callejones: del punto medio de una ronda al de la siguiente (entre los mismos radiales).
    const auto steps = [](int from, int to) { return ((to - from) % 16 + 16) % 16; };
    for (int ring = 1; ring + 2 < NR + 1 && ring + 1 < NR; ++ring)
        for (const auto& mi_ : ring_mid[ring]) {
            const int a = static_cast<int>(mi_[1].x), b = static_cast<int>(mi_[1].z);
            for (const auto& mo : ring_mid[ring + 1]) {
                const int c2 = static_cast<int>(mo[1].x), d2 = static_cast<int>(mo[1].z);
                const int span = steps(a, b) == 0 ? 16 : steps(a, b);
                const bool inside = steps(a, c2) < span && steps(a, d2) <= span && steps(a, d2) > 0;
                if (!inside) continue;
                const Node& m0 = mi_[0];
                const Node& m1 = mo[0];
                const float dd = (m1.x - m0.x) * (m1.x - m0.x) + (m1.z - m0.z) * (m1.z - m0.z);
                if (dd > fR * fR * 0.25f) continue;
                // Solo si la manzana es ancha (arco de mas de 46 m).
                const Node& na = node[a][ring];
                const Node& nb = node[b][ring];
                if ((na.x - nb.x) * (na.x - nb.x) + (na.z - nb.z) * (na.z - nb.z) < 46.0f * 46.0f) continue;
                if (!blocked(m0.x, m0.z, m1.x, m1.z)) edges.push_back(Edge{m0.x, m0.z, m1.x, m1.z, m0.h, m1.h, 1.3f, 3});
            }
        }
    // Pintar: callejones, rondas, radiales y avenidas (las de mas rango mandan en los cruces).
    for (int pass = 3; pass >= 0; --pass)
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const Edge& e = edges[i];
            const int order = e.kind == 3 ? 3 : (e.kind == 1 ? 2 : (e.kind == 2 ? 1 : 0));
            if (order != pass) continue;
            const BlockId surf = e.kind == 3 ? lane : (e.kind == 0 || e.kind == 4 ? pave : cobble);
            const BlockId alt = e.kind == 3 ? B::DirtPath : (e.kind == 0 ? B::Cobblestone : B::Gravel);
            const float bend = e.kind == 0 || e.kind == 4 ? 2.0f : (e.kind == 3 ? 3.0f : 3.5f);
            metroStreet(s, occ, hf, e.ax, e.az, e.ha, e.bx, e.bz, e.hb, e.half, surf, alt, bend, static_cast<std::uint32_t>(i * 7 + 11));
        }
    // --- Murallas: torres y puertas ---
    for (int k = 0; k < 16; ++k) {
        float x, z;
        wallPt(k, x, z);
        const int gh = groundOr(s, static_cast<int>(std::floor(x)), static_cast<int>(std::floor(z)), base);
        if (k % 4 == 0) {
            BuildContext ctx = context(BuildStyle::City, B::RoofSlate, 1100 + k);
            build::gatehouse(ctx, 15, kWallH + 4, 5);
            const int rot = rotFacing(kD16[k][0], kD16[k][1]);
            const int sx = (rot & 1) ? ctx.bp.d : ctx.bp.w, sz = (rot & 1) ? ctx.bp.w : ctx.bp.d;
            const int x0 = static_cast<int>(std::lround(x - static_cast<float>(sx) * 0.5f)), z0 = static_cast<int>(std::lround(z - static_cast<float>(sz) * 0.5f));
            add(s, ctx, x0, gh, z0, rot);
            occ.fill(Rect{x0, z0, x0 + sx - 1, z0 + sz - 1}, Occ::Rampart);
            metroPad(s, occ, Rect{x0, z0, x0 + sx - 1, z0 + sz - 1}, gh);
        } else {
            BuildContext ctx = context(BuildStyle::City, capital ? B::RoofBlue : B::RoofRed, 1200 + k);
            build::tower(ctx, 9, kWallH + 7);
            const int size = ctx.bp.w;
            const int rot = rotFacing(fcx - x, fcz - z);
            add(s, ctx, static_cast<int>(std::lround(x)) - size / 2, gh, static_cast<int>(std::lround(z)) - size / 2, rot);
            const Rect tr{static_cast<int>(std::lround(x)) - size / 2, static_cast<int>(std::lround(z)) - size / 2,
                          static_cast<int>(std::lround(x)) - size / 2 + size - 1, static_cast<int>(std::lround(z)) - size / 2 + size - 1};
            occ.fill(tr, Occ::Rampart);
            metroPad(s, occ, tr, gh);
        }
    }
    // Torres a mitad de los tramos largos (la capital).
    if (capital)
        for (int k = 0; k < 16; ++k) {
            if (k % 4 == 3 || k % 4 == 0) continue;
            float ax, az, bx, bz;
            wallPt(k, ax, az);
            wallPt((k + 1) % 16, bx, bz);
            const float x = (ax + bx) * 0.5f, z = (az + bz) * 0.5f;
            const int gh = groundOr(s, static_cast<int>(std::floor(x)), static_cast<int>(std::floor(z)), base);
            BuildContext ctx = context(BuildStyle::City, B::RoofBlue, 1250 + k);
            build::tower(ctx, 7, kWallH + 4);
            const int size = ctx.bp.w;
            add(s, ctx, static_cast<int>(std::lround(x)) - size / 2, gh, static_cast<int>(std::lround(z)) - size / 2, rotFacing(fcx - x, fcz - z));
        }
    // --- Castillo ---
    {
        BuildContext ctx = context(BuildStyle::City, capital ? B::RoofBlue : B::RoofSlate, 1300);
        build::castle(ctx, castle_w, castle_d, true);
        const int rot = castle_rot;
        const int sx = (rot & 1) ? castle_d : castle_w, sz = (rot & 1) ? castle_w : castle_d;
        const int x0 = static_cast<int>(std::lround(Cx - static_cast<float>(sx) * 0.5f)), z0 = static_cast<int>(std::lround(Cz - static_cast<float>(sz) * 0.5f));
        add(s, ctx, x0, castle_h - 6, z0, rot);
        const Rect r{x0, z0, x0 + sx - 1, z0 + sz - 1};
        occ.fill(r, Occ::Building);
        metroPad(s, occ, Rect{r.x0 - 1, r.z0 - 1, r.x1 + 1, r.z1 + 1}, castle_h);
    }
    // --- Catedral y cementerio ---
    {
        BuildContext ctx = context(BuildStyle::City, capital ? B::RoofBlue : B::RoofSlate, 1350);
        build::cathedral(ctx);
        const int rot = cath_rot;
        const int sx = (rot & 1) ? kCathD : kCathW, sz = (rot & 1) ? kCathW : kCathD;
        const int x0 = static_cast<int>(std::lround(Tcx - static_cast<float>(sx) * 0.5f)), z0 = static_cast<int>(std::lround(Tcz - static_cast<float>(sz) * 0.5f));
        add(s, ctx, x0, tq.h - build::kCryptDepth, z0, rot);
        const Rect r{x0, z0, x0 + sx - 1, z0 + sz - 1};
        occ.fill(r, Occ::Building);
        metroPad(s, occ, Rect{r.x0 - 1, r.z0 - 1, r.x1 + 1, r.z1 + 1}, tq.h);
        for (int side = -1; side <= 1; side += 2) {
            BuildContext gc = context(bstyle, B::RoofSlate, 1360 + side);
            const float off = kCathW * 0.5f + 2.0f + 9.5f;
            const float gx = Tcx - toz * off * static_cast<float>(side), gz = Tcz + tox * off * static_cast<float>(side);
            if (metroPlace(s, occ, hf, gc, [&](BuildContext& c) { build::graveyard(c, 17, 21); }, 17, 21, gx, gz, -tox, -toz,
                           hillHeight(hf, gx, gz), -6, 0, 5))
                break;
        }
    }
    // --- Plaza del mercado: fuente, puestos en corros, farolas y bancos ---
    {
        const int fs = capital ? 13 : 11;
        BuildContext ctx = context(BuildStyle::City, B::RoofRed, 1400);
        build::fountain(ctx, fs);
        add(s, ctx, cx - fs / 2, base - 1, cz - fs / 2, 0);
        addPoint(s, "plaza", fcx, static_cast<float>(base + 1), fcz + static_cast<float>(fs / 2 + 2));
        static const char* const kShops[] = {"armas", "armaduras", "pociones", "monturas", "general", "pociones", "armas", "general"};
        static const BlockId kCloth[] = {B::ClothRed, B::ClothBlue, B::ClothYellow, B::ClothRed, B::ClothBlue, B::ClothYellow, B::ClothWhite, B::ClothRed};
        int made = 0;
        for (float rr = P - 5.0f; rr > static_cast<float>(fs) * 0.5f + 4.0f; rr -= 8.0f)
            for (int k = 0; k < 16; ++k) {
                for (int h2 = 0; h2 < 2; ++h2) {
                    // Entre dos direcciones (no en la boca de las calles).
                    const float dx = (kD16[k][0] + kD16[(k + 1) % 16][0] * (h2 ? 1.0f : 0.0f)), dz = (kD16[k][1] + kD16[(k + 1) % 16][1] * (h2 ? 1.0f : 0.0f));
                    const float dl = std::sqrt(dx * dx + dz * dz);
                    if (h2 == 0 && (k % 2 == 0)) continue;  // boca de calle
                    const float ux = dx / dl, uz = dz / dl;
                    BuildContext sc = context(BuildStyle::City, B::RoofRed, 1500 + made);
                    build::stall(sc, kCloth[made % 8], kShops[made % 8]);
                    const int rot = rotFacing(-ux, -uz);
                    const int sx = (rot & 1) ? sc.bp.d : sc.bp.w, sz = (rot & 1) ? sc.bp.w : sc.bp.d;
                    const int x0 = static_cast<int>(std::lround(fcx + ux * rr - static_cast<float>(sx) * 0.5f));
                    const int z0 = static_cast<int>(std::lround(fcz + uz * rr - static_cast<float>(sz) * 0.5f));
                    add(s, sc, x0, base, z0, rot);
                    ++made;
                }
            }
        for (int k = 1; k < 16; k += 2) {
            const int x = static_cast<int>(std::lround(fcx + kD16[k][0] * (P - 1.0f))), z = static_cast<int>(std::lround(fcz + kD16[k][1] * (P - 1.0f)));
            BuildContext lc = context(BuildStyle::City, B::RoofRed, 1600 + k);
            build::streetLamp(lc);
            add(s, lc, x, base, z, 0);
        }
    }
    // Pozos en las plazuelas de las avenidas y en la plaza de la catedral (farolas).
    for (int k = 0; k < 16; k += 4) {
        const Node& n = node[k][2];
        BuildContext ctx = context(bstyle, B::RoofRed, 1700 + k);
        build::well(ctx);
        add(s, ctx, static_cast<int>(std::floor(n.x)) - 2 + static_cast<int>(kD16[(k + 4) % 16][0] * 4.0f),
            n.h - 3, static_cast<int>(std::floor(n.z)) - 2 + static_cast<int>(kD16[(k + 4) % 16][1] * 4.0f), 0);
    }
    for (int i = 0; i < 4; ++i) {
        const int k = (kt + 2 + i * 4) % 16;
        BuildContext lc = context(BuildStyle::City, B::RoofRed, 1750 + i);
        build::streetLamp(lc);
        add(s, lc, static_cast<int>(std::lround(tq.x + kD16[k][0] * (Tq - 1.0f))), tq.h, static_cast<int>(std::lround(tq.z + kD16[k][1] * (Tq - 1.0f))), 0);
    }
    // --- Barrios ---
    int sector[8];
    const int kc8 = kc / 2;
    {
        const int order[8] = {DNoble, DResidential, DArtisan, DArtisan, DTemple, DPoor, DPoor, DNoble};
        for (int i = 0; i < 8; ++i) sector[(kc8 + i) % 8] = order[i];
    }
    // Puerto: la puerta que da al agua (mar o lago a menos de 110 m de la muralla).
    int harbor = -1, water_at = 0;
    for (int g = 0; g < 4 && harbor < 0; ++g) {
        const int k = g * 4;
        for (int t = R + 12; t < R + 110; t += 2) {
            const int x = static_cast<int>(fcx + kD16[k][0] * static_cast<float>(t)), z = static_cast<int>(fcz + kD16[k][1] * static_cast<float>(t));
            const auto c = col(x, z);
            if (c.height < sea - 1 || (c.lake && c.water > c.height)) {
                harbor = g;
                water_at = t;
                break;
            }
        }
    }
    if (harbor >= 0) sector[harbor * 2] = DHarbor;
    const auto districtAt = [&](float x, float z) {
        const float dx = x - fcx, dz = z - fcz;
        const float d2 = dx * dx + dz * dz;
        if (d2 > (fR - 6.0f) * (fR - 6.0f)) return static_cast<int>(DOutside);
        if (d2 < (fR * 0.33f) * (fR * 0.33f)) return static_cast<int>(DMarket);
        if ((x - Cx) * (x - Cx) + (z - Cz) * (z - Cz) < (rc + 30.0f) * (rc + 30.0f)) return static_cast<int>(DNoble);
        return sector[nearestDir16(dx, dz, 2) / 2];
    };
    std::vector<int> queue[8];
    queue[DMarket] = {MGuildMerchants, MBank, MTavern, MShopGeneral, MGuildMages, MShopPotions, MShopArmor, MTavern};
    queue[DNoble] = {MBarracks, MYard, MManor, MManor, MGarden, MManor, MStable};
    queue[DArtisan] = {MSmithy, MWorkCarpenter, MWorkTanner, MGuildSmiths, MSmithy, MWorkWeaver, MWorkPotter, MStable, MSmithy, MTavern};
    queue[DPoor] = {MTavern, MChapel, MStable, MWorkTanner, MGuildThieves};
    queue[DResidential] = {MTavern, MShopGeneral, MGarden, MShopPotions};
    queue[DHarbor] = {MWarehouse, MWarehouse, MTavern, MWarehouse, MShopGeneral};
    queue[DTemple] = {MGraveyard, MChapel, MShopPotions, MGarden};
    queue[DOutside] = {MTavern, MStable, MFarmhouse, MFarmhouse};
    if (capital) {
        for (int k : {MGuildSmiths, MBank, MTavern, MShopArmor, MGuildThieves, MShopGeneral}) queue[DMarket].push_back(k);
        for (int k : {MManor, MManor, MBarracks, MYard, MManor}) queue[DNoble].push_back(k);
        for (int k : {MWorkCarpenter, MSmithy, MWorkWeaver}) queue[DArtisan].push_back(k);
        for (int k : {MTavern, MChapel}) queue[DPoor].push_back(k);
    }
    std::size_t qi[8] = {};
    static const char* const kTags[] = {"ciudadano", "artesano", "comerciante", "aldeano", "panadero", "sastre", "carpintero", "mensajero",
                                        "erudito", "nino", "guardia", "lavandera"};
    static const char* const kRichTags[] = {"noble", "comerciante", "erudito", "dama", "mayordomo"};
    static const char* const kPoorTags[] = {"aldeano", "mendigo", "pescador", "lavandera", "nino", "ladron"};
    const auto roofFor = [&](int dist, std::uint32_t h) -> BlockId {
        static const BlockId kWarm[] = {B::RoofRed, B::RoofOrange, B::RoofRed, B::RoofGreen};
        static const BlockId kRich[] = {B::RoofBlue, B::RoofSlate, B::RoofPurple, B::RoofBlue};
        if (bstyle == BuildStyle::Snow) return (h & 1) ? B::RoofSlate : B::RoofBlue;
        switch (dist) {
            case DNoble: return kRich[h % 4];
            case DPoor: return (h % 3) ? B::Thatch : B::RoofOrange;
            case DHarbor: return (h % 2) ? B::Thatch : B::RoofBlue;
            default: return kWarm[h % 4];
        }
    };
    int placed = 0;
    // Recorre un tramo y pone edificios a los dos lados (filas apretadas).
    const auto fillStreet = [&](const Road& road, int only_side) {
        const float vx = road.x1 - road.x0, vz = road.z1 - road.z0;
        const float len = std::sqrt(vx * vx + vz * vz);
        if (len < 6.0f) return;
        const float ux = vx / len, uz = vz / len;
        for (int side = -1; side <= 1; side += 2) {
            if (only_side != 0 && side != only_side) continue;
            const float nx = -uz * static_cast<float>(side), nz = ux * static_cast<float>(side);
            float cursor = 1.0f;
            int fails = 0;
            while (cursor < len - 3.0f) {
                const float mx = road.x0 + ux * (cursor + 4.0f) + nx * 8.0f, mz = road.z0 + uz * (cursor + 4.0f) + nz * 8.0f;
                const int dist = districtAt(mx, mz);
                const std::uint32_t h = hashOf(seed, static_cast<int>(mx * 2.0f) + mi * 977, static_cast<int>(mz * 2.0f) + mj * 991, 0x5EED);
                const float pick = static_cast<float>(h & 0xFFFF) / 65536.0f;
                int kind = MTown;
                const bool special = qi[dist] < queue[dist].size() && fails == 0;
                if (special) kind = queue[dist][qi[dist]];
                else if (dist == DPoor) kind = pick < 0.85f ? MSmall : MTown;
                else if (dist == DArtisan && pick < 0.22f) kind = (h >> 16) % 2 ? MWorkCarpenter : MWorkTanner;
                else if (dist == DHarbor && pick < 0.3f) kind = MWarehouse;
                else if (dist == DOutside) kind = pick < 0.5f ? MFarmhouse : MGarden;
                else if ((dist == DResidential || dist == DTemple || dist == DNoble) && pick < 0.07f) kind = MGarden;
                // Tamano (sin aleros) y pisos segun el barrio.
                int hw = 8, hd = 9, floors = 2;
                const auto rr = [&](int lo, int hi, int salt) { return lo + static_cast<int>(((h >> salt) & 0xFF) % static_cast<std::uint32_t>(hi - lo + 1)); };
                switch (dist) {
                    case DMarket: hw = rr(8, 11, 3); hd = rr(10, 13, 7); floors = rr(3, capital ? 4 : 3, 11) + ((h >> 21) % 3 == 0 ? 1 : 0); break;
                    case DNoble: hw = rr(10, 12, 3); hd = rr(10, 11, 7); floors = 3; break;
                    case DArtisan: hw = rr(7, 9, 3); hd = rr(8, 10, 7); floors = rr(2, 3, 11); break;
                    case DPoor: hw = rr(5, 7, 3); hd = rr(6, 8, 7); floors = rr(1, 2, 11); break;
                    case DHarbor: hw = rr(7, 9, 3); hd = rr(7, 9, 7); floors = rr(1, 2, 11); break;
                    case DOutside: hw = rr(7, 8, 3); hd = rr(7, 8, 7); floors = 1; break;
                    default: hw = rr(7, 10, 3); hd = rr(8, 10, 7); floors = rr(2, 3, 11); break;
                }
                if (fails >= 1 && (kind == MTown || kind == MSmall)) hw = std::max(5, hw - 2);  // un hueco estrecho: casa estrecha
                floors = std::min(floors, 4);
                if (bstyle == BuildStyle::Desert) floors = std::min(floors, 3);
                if (kind == MSmall) floors = std::min(floors, 2);
                int bw = hw + 2, bd = hd + 2, yoff = 0;
                bool cellar = false;
                switch (kind) {
                    case MTavern: bw = 15; bd = 13; break;
                    case MSmithy: bw = 12; bd = 10; break;
                    case MShopGeneral: case MShopPotions: case MShopArmor: bw = 11; bd = 10; break;
                    case MStable: bw = 13; bd = 11; break;
                    case MChapel: bw = 13; bd = 21; break;
                    case MGuildMerchants: case MGuildSmiths: case MGuildMages: case MGuildThieves: bw = 15; bd = 13; break;
                    case MBank: bw = 13; bd = 12; yoff = -6; break;
                    case MManor: bw = 17; bd = 21; break;
                    case MGraveyard: bw = 15; bd = 17; yoff = -6; break;
                    case MBarracks: bw = 17; bd = 11; break;
                    case MYard: bw = 17; bd = 13; break;
                    case MWarehouse: bw = 13; bd = 11; break;
                    case MWorkTanner: case MWorkCarpenter: case MWorkWeaver: case MWorkPotter: bw = 12; bd = 10; break;
                    case MGarden: bw = rr(9, 13, 13); bd = rr(9, 11, 17); break;
                    case MFarmhouse: bw = hw + 2; bd = hd + 2; break;
                    default:
                        cellar = (kind == MTown) && ((h >> 9) % 10 < (dist == DMarket ? 4u : 2u)) && hd >= 8;
                        if (cellar) yoff = -5;
                        break;
                }
                const int gap = dist == DNoble ? 2 : (dist == DArtisan || dist == DOutside ? 1 : 0);
                const float setback = dist == DOutside ? 3.0f : 0.0f;
                const int frot = rotFacing(-nx, -nz);
                const float fsx = static_cast<float>((frot & 1) ? bd : bw), fsz = static_cast<float>((frot & 1) ? bw : bd);
                const float along = std::abs(ux) * fsx + std::abs(uz) * fsz, depth = std::abs(nx) * fsx + std::abs(nz) * fsz;
                const float dd = road.half_width + 0.5f + setback + depth * 0.5f;
                const float t = cursor + along * 0.5f;
                const float px = road.x0 + ux * t, pz = road.z0 + uz * t;
                const int floor_y = groundOr(s, static_cast<int>(std::floor(px)), static_cast<int>(std::floor(pz)), base);
                BuildStyle st = bstyle == BuildStyle::Desert ? BuildStyle::Desert
                                                             : ((dist == DMarket || dist == DNoble) ? BuildStyle::City
                                                                                                    : (bstyle == BuildStyle::Snow ? BuildStyle::Snow : (dist == DHarbor ? BuildStyle::Beach : BuildStyle::Temperate)));
                if (dist == DResidential && bstyle != BuildStyle::Snow && bstyle != BuildStyle::Desert && (h >> 13) % 2) st = BuildStyle::City;
                BuildContext ctx = context(st, roofFor(dist, h >> 4), 2000 + placed * 3 + (side > 0 ? 1 : 0));
                const std::string tag = dist == DNoble ? kRichTags[(h >> 5) % 5] : (dist == DPoor || dist == DHarbor ? kPoorTags[(h >> 5) % 6] : kTags[(h >> 5) % 12]);
                const int variant = static_cast<int>((h >> 8) % 1000);
                const auto fn = [&](BuildContext& c) {
                    switch (kind) {
                        case MTavern: build::tavern(c); break;
                        case MSmithy: build::smithy(c, true); break;
                        case MShopGeneral: build::shop(c, "general"); break;
                        case MShopPotions: build::shop(c, "pociones"); break;
                        case MShopArmor: build::shop(c, "armaduras"); break;
                        case MStable: build::stable(c); break;
                        case MChapel: c.roof = B::RoofBlue; build::temple(c); break;
                        case MGuildMerchants: build::guildHall(c, "mercaderes"); break;
                        case MGuildSmiths: build::guildHall(c, "herreros"); break;
                        case MGuildMages: build::guildHall(c, "magos"); break;
                        case MGuildThieves: build::guildHall(c, "ladrones"); break;
                        case MBank: build::bank(c); break;
                        case MManor: build::manor(c); break;
                        case MBarracks: build::barracks(c); break;
                        case MYard: build::trainingYard(c, bw, bd); break;
                        case MWarehouse: build::warehouse(c); break;
                        case MWorkTanner: build::workshop(c, "curtidor"); break;
                        case MWorkCarpenter: build::workshop(c, "carpintero"); break;
                        case MWorkWeaver: build::workshop(c, "tejedor"); break;
                        case MWorkPotter: build::workshop(c, "alfarero"); break;
                        case MGarden: build::garden(c, bw, bd); break;
                        case MGraveyard: build::graveyard(c, bw, bd); break;
                        case MSmall:
                            if (c.style == BuildStyle::City) c.style = BuildStyle::Temperate;
                            build::house(c, hw, hd, floors, tag);
                            break;
                        default: build::townhouse(c, hw, hd, floors, tag, variant, cellar); break;
                    }
                };
                const bool ok = metroPlace(s, occ, hf, ctx, fn, bw, bd, px + nx * dd, pz + nz * dd, -nx, -nz, floor_y, yoff, gap, dist == DOutside ? 4 : 6);
                if (ok) {
                    ++placed;
                    if (special) ++qi[dist];
                    cursor += along + static_cast<float>(gap);
                    fails = 0;
                } else {
                    ++fails;
                    cursor += fails == 1 ? 0.0f : 1.0f;  // primero prueba una casa normal en el mismo sitio
                }
            }
        }
    };
    // Primero las fachadas de la plaza del mercado (mirando al centro).
    {
        const int nseg = 24;
        for (int i = 0; i < nseg; ++i) {
            const int k0 = (i * 16) / nseg, k1 = ((i + 1) * 16) / nseg;
            (void)k0;
            (void)k1;
        }
        for (int k = 0; k < 16; ++k) {
            const int k2 = (k + 1) % 16;
            const float r = P + 0.5f;
            Road pr{fcx + kD16[k2][0] * r, fcz + kD16[k2][1] * r, fcx + kD16[k][0] * r, fcz + kD16[k][1] * r, 0.0f, B::Air, B::Air, 0.0f};
            fillStreet(pr, 1);
        }
    }
    // Calles por orden de rango (avenidas, rondas, radiales, callejones; fuera al final).
    const std::size_t nroads = s.roads.size();
    for (int pass = 0; pass < 5; ++pass)
        for (std::size_t i = 0; i < nroads; ++i) {
            const Road road = s.roads[i];
            const float hw = road.half_width;
            const bool out = static_cast<int>(districtAt((road.x0 + road.x1) * 0.5f, (road.z0 + road.z1) * 0.5f)) == DOutside;
            const int rank = out ? 4 : (hw >= 2.5f ? 0 : (hw >= 2.0f ? 1 : (hw >= 1.5f ? 2 : 3)));
            if (rank != pass) continue;
            fillStreet(road, 0);
        }
    // Relleno de manzana: en lo que queda libre, casas que dan a la calle que tengan delante
    // (manzanas irregulares: las filas de los tramos con recodo dejan huecos).
    {
        const auto streetish = [&](int x, int z) {
            const std::uint8_t v = occ.at(x, z);
            return v == Occ::Street || v == Occ::Square;
        };
        for (int z = cz - R + 6; z <= cz + R - 6; z += 2)
            for (int x = cx - R + 6; x <= cx + R - 6; x += 2) {
                if (occ.at(x, z) != Occ::Free) continue;
                const float fx = static_cast<float>(x) - fcx, fz = static_cast<float>(z) - fcz;
                if (fx * fx + fz * fz > (fR - 9.0f) * (fR - 9.0f)) continue;
                const int dist = districtAt(static_cast<float>(x), static_cast<float>(z));
                const std::uint32_t h = hashOf(seed, x + mi * 977, z + mj * 991, 0x1F11);
                const auto rr = [&](int lo, int hi, int salt) { return lo + static_cast<int>(((h >> salt) & 0xFF) % static_cast<std::uint32_t>(hi - lo + 1)); };
                int hw = dist == DPoor ? rr(5, 7, 3) : (dist == DMarket || dist == DNoble ? rr(8, 10, 3) : rr(6, 9, 3));
                const int hd = dist == DPoor ? rr(6, 8, 7) : rr(7, 10, 7);
                for (int shrink = 0; shrink < 3; ++shrink, hw = std::max(5, hw - 2)) {
                    bool done = false;
                    for (int f = 0; f < 4 && !done; ++f) {
                        // Rectangulo con la fachada hacia kFront[f] y la esquina en (x, z).
                        const int sx = (f & 1) ? hd + 2 : hw + 2, sz = (f & 1) ? hw + 2 : hd + 2;
                        const Rect r{x, z, x + sx - 1, z + sz - 1};
                        if (!occ.rectFree(r, 0)) continue;
                        // Delante (1 o 2 celdas) tiene que haber calle en la mayor parte de la fachada.
                        int hits = 0, total = 0, fy = 0;
                        const int fxs = kFront[f][0], fzs = kFront[f][1];
                        for (int i = 0; i < (f & 1 ? sz : sx); ++i) {
                            const int bx = f == 1 ? r.x1 : (f == 3 ? r.x0 : r.x0 + i), bz = f == 0 ? r.z0 : (f == 2 ? r.z1 : r.z0 + i);
                            ++total;
                            for (int dd2 = 1; dd2 <= 3; ++dd2)
                                if (streetish(bx + fxs * dd2, bz + fzs * dd2)) {
                                    if (hits == 0) fy = groundOr(s, bx + fxs * dd2, bz + fzs * dd2, base);
                                    ++hits;
                                    break;
                                }
                        }
                        if (hits * 2 < total) continue;
                        const BuildStyle st = bstyle == BuildStyle::Desert ? BuildStyle::Desert
                                                                           : (dist == DMarket || dist == DNoble ? BuildStyle::City
                                                                                                                 : (bstyle == BuildStyle::Snow ? BuildStyle::Snow : BuildStyle::Temperate));
                        BuildContext ctx = context(st, roofFor(dist, h >> 4), 2600 + placed * 3);
                        int floors = dist == DPoor ? rr(1, 2, 11) : (dist == DMarket ? rr(3, 4, 11) : rr(2, 3, 11));
                        if (bstyle == BuildStyle::Desert) floors = std::min(floors, 3);
                        const std::string tag = dist == DNoble ? kRichTags[(h >> 5) % 5] : (dist == DPoor ? kPoorTags[(h >> 5) % 6] : kTags[(h >> 5) % 12]);
                        const int variant = static_cast<int>((h >> 8) % 1000);
                        const int bw = hw + 2, bd = hd + 2;
                        const auto fn = [&](BuildContext& c) {
                            if (dist == DPoor) {
                                if (c.style == BuildStyle::City) c.style = BuildStyle::Temperate;
                                build::house(c, hw, hd, std::min(floors, 2), tag);
                            } else {
                                build::townhouse(c, hw, hd, floors, tag, variant, false);
                            }
                        };
                        if (metroPlace(s, occ, hf, ctx, fn, bw, bd, static_cast<float>(r.x0 + r.x1 + 1) * 0.5f, static_cast<float>(r.z0 + r.z1 + 1) * 0.5f,
                                       static_cast<float>(fxs), static_cast<float>(fzs), fy, 0, 0, 6)) {
                            ++placed;
                            done = true;
                        }
                    }
                    if (done) break;
                }
            }
    }
    // Patios de manzana: jardines y arboles en lo que queda libre dentro de la muralla.
    {
        int k = 0;
        for (int z = cz - R + 8; z <= cz + R - 8; z += 2)
            for (int x = cx - R + 8; x <= cx + R - 8; x += 2) {
                const float dx = static_cast<float>(x) - fcx, dz = static_cast<float>(z) - fcz;
                if (dx * dx + dz * dz > (fR - 12.0f) * (fR - 12.0f)) continue;
                if (occ.at(x, z) != Occ::Free) continue;
                for (int gw = 13; gw >= 7; gw -= 3) {
                    const Rect r{x, z, x + gw - 1, z + gw - 1};
                    if (!occ.rectFree(r, 0)) continue;
                    const int gy = groundOr(s, x + gw / 2, z + gw / 2, base);
                    occ.fill(r, Occ::Building);
                    BuildContext gc = context(bstyle == BuildStyle::Desert ? BuildStyle::Desert : BuildStyle::Temperate, B::RoofRed, 3300 + k++);
                    if (gw >= 10) build::garden(gc, gw, gw);
                    else build::tree(gc);
                    add(s, gc, x, gy, z, static_cast<int>(hashOf(seed, x, z, 0x6A2E) & 3u));
                    metroPad(s, occ, r, gy);
                    break;
                }
            }
    }
    // --- Tuneles de los ladrones: pozo seco en el barrio pobre -> guarida -> sala del puzle ->
    // salida por otro pozo fuera de la muralla ---
    {
        int sp = 0;
        for (int i = 0; i < 8; ++i)
            if (sector[i] == DPoor) {
                sp = i;
                break;
            }
        const int ka = nearestDir16(kD16[sp * 2][0], kD16[sp * 2][1], 4);
        const int ax = static_cast<int>(kD16[ka][0]), az = static_cast<int>(kD16[ka][1]);
        const int px_ = -az, pz_ = ax;  // perpendicular
        for (int attempt = 0; attempt < 6; ++attempt) {
            const int off = 14 + attempt * 6;
            const int u0 = static_cast<int>(fR * 0.5f), u1 = R + 24;
            const int ex = cx + ax * u0 + px_ * off, ez = cz + az * u0 + pz_ * off;
            const int xx = cx + ax * u1 + px_ * off, xz = cz + az * u1 + pz_ * off;
            const Rect re{ex - 2, ez - 2, ex + 2, ez + 2}, rx_{xx - 2, xz - 2, xx + 2, xz + 2};
            if (!occ.rectFree(re, 0) || !occ.rectFree(rx_, 0)) continue;
            int lo = 1 << 20;
            for (int u = u0; u <= u1; u += 4) lo = std::min(lo, groundOr(s, cx + ax * u + px_ * off, cz + az * u + pz_ * off, base));
            const int yT = lo - 13;
            if (yT < 8) break;
            const int ge = groundOr(s, ex, ez, base), gx = groundOr(s, xx, xz, base);
            const auto alongPiece = [&](BuildContext& ctx, int ustart, int ulen, int y) {
                // Pieza de ancho impar centrada en el eje, de ustart a ustart + ulen - 1 a lo largo.
                const int rot = az == 1 ? 0 : (az == -1 ? 2 : (ax == 1 ? 3 : 1));
                const int wpx = ctx.bp.w;
                int x0 = 0, z0 = 0;
                const int ua = ustart, ub = ustart + ulen - 1;
                const int wa = -(wpx / 2), wb = wpx / 2;
                const int xa = cx + ax * ua + px_ * (off + wa), xb = cx + ax * ub + px_ * (off + wb);
                const int za = cz + az * ua + pz_ * (off + wa), zb = cz + az * ub + pz_ * (off + wb);
                x0 = std::min(xa, xb);
                z0 = std::min(za, zb);
                add(s, ctx, x0, y, z0, rot);
            };
            {
                BuildContext w1 = context(BuildStyle::Temperate, B::RoofRed, 4500);
                build::dryWell(w1, ge - yT);
                add(s, w1, ex - 2, yT, ez - 2, 0);
                occ.fill(re, Occ::Reserved);
                metroPad(s, occ, re, ge);
                BuildContext w2 = context(BuildStyle::Temperate, B::Thatch, 4501);
                build::dryWell(w2, gx - yT);
                add(s, w2, xx - 2, yT, xz - 2, 0);
                occ.fill(rx_, Occ::Reserved);
                metroPad(s, occ, rx_, gx);
            }
            const int L = u1 - u0;
            const int r1 = u0 + L / 3 - 5, r2 = u0 + (2 * L) / 3 - 5;  // inicio de cada sala (11 de largo)
            int u = u0 + 3;
            int salt = 0;
            for (int room = 0; room < 2; ++room) {
                const int rs = room == 0 ? r1 : r2;
                if (rs - u > 0) {
                    BuildContext tc = context(BuildStyle::Temperate, B::RoofRed, 4510 + salt++);
                    build::tunnel(tc, rs - u);
                    alongPiece(tc, u, rs - u, yT);
                }
                BuildContext rcx = context(BuildStyle::Temperate, B::RoofRed, 4520 + room);
                build::undergroundRoom(rcx, 9, 9, room == 0 ? "guarida" : "puzle", room == 0 ? 2 : 3);
                alongPiece(rcx, rs, 11, yT);
                u = rs + 11;
            }
            if (u1 - 3 - u > 0) {
                BuildContext tc = context(BuildStyle::Temperate, B::RoofRed, 4530);
                build::tunnel(tc, u1 - 2 - u);
                alongPiece(tc, u, u1 - 2 - u, yT);
            }
            break;
        }
    }
    // --- Puerto ---
    if (harbor >= 0) {
        const int k = harbor * 4;
        const int start = water_at - 3;
        for (int i = -1; i <= 1; i += 2) {
            BuildContext pc = context(BuildStyle::Beach, B::Thatch, 3800 + i);
            build::pier(pc, 22, sea);
            const float bx = fcx + kD16[k][0] * static_cast<float>(start) - kD16[k][1] * 12.0f * static_cast<float>(i);
            const float bz = fcz + kD16[k][1] * static_cast<float>(start) + kD16[k][0] * 12.0f * static_cast<float>(i);
            const int rot = rotFacing(-kD16[k][0], -kD16[k][1]);
            const int sx = (rot & 1) ? 22 : 5, sz = (rot & 1) ? 5 : 22;
            int x0 = static_cast<int>(std::lround(bx)) - sx / 2, z0 = static_cast<int>(std::lround(bz)) - sz / 2;
            x0 += static_cast<int>(kD16[k][0]) * 11;
            z0 += static_cast<int>(kD16[k][1]) * 11;
            add(s, pc, x0, sea - 8, z0, rot);
        }
    }
    // --- Fuera: campos, huertos, molinos y granjas ---
    {
        const int want = capital ? 46 : 30;
        int made = 0;
        for (int i = 0; i < want * 8 && made < want; ++i) {
            const int k = static_cast<int>(mr.next() % 16u);
            const float dist = fR + 16.0f + mr.f() * 58.0f;
            const float side = (mr.f() - 0.5f) * fR * 0.36f;
            const float x = fcx + kD16[k][0] * dist - kD16[k][1] * side, z = fcz + kD16[k][1] * dist + kD16[k][0] * side;
            const float pick = mr.f();
            int w = 0, d = 0;
            if (pick < 0.55f) w = mr.range(11, 17), d = mr.range(8, 11);
            else if (pick < 0.8f) w = mr.range(11, 15), d = mr.range(11, 15);
            else if (pick < 0.9f) w = d = 9;
            else w = 9, d = 9;
            const int x0 = static_cast<int>(x) - w / 2, z0 = static_cast<int>(z) - d / 2;
            const Rect r{x0, z0, x0 + w - 1, z0 + d - 1};
            if (!occ.rectFree(r, 2)) continue;
            const int gy = terraceAt(hf, x0 + w / 2, z0 + d / 2);
            bool flat = true;
            for (int c2 = 0; c2 < 4 && flat; ++c2) {
                const int qx = c2 & 1 ? r.x1 : r.x0, qz = c2 & 2 ? r.z1 : r.z0;
                if (std::abs(hillHeight(hf, static_cast<float>(qx), static_cast<float>(qz)) - gy) > 3) flat = false;
            }
            if (!flat) continue;
            occ.fill(r, Occ::Building);
            BuildContext fc = context(bstyle == BuildStyle::Desert ? BuildStyle::Desert : BuildStyle::Temperate, B::Thatch, 3900 + i);
            if (pick < 0.55f) {
                if (bstyle == BuildStyle::Desert) build::orchard(fc, w, d);
                else build::farm(fc, w, d);
            } else if (pick < 0.8f) {
                build::orchard(fc, w, d);
            } else if (pick < 0.9f) {
                build::windmill(fc);
            } else {
                build::house(fc, 7, 7, 1, "granjero");
            }
            add(s, fc, x0, gy, z0, static_cast<int>(mr.next() & 3u));
            metroPad(s, occ, r, gy);
            ++made;
        }
    }
    groundSteps(s);
    // Guardias en las puertas y en la plaza.
    for (int g = 0; g < 4; ++g) addPoint(s, "npc", gateX[g] - kD16[g * 4][0] * 8.0f, static_cast<float>(gateH[g] + 1), gateZ[g] - kD16[g * 4][1] * 8.0f, "guardia", g);
    // Menos puntos de aldeano y cama (son miles): el juego no necesita tantos.
    {
        std::vector<StructurePoint> keep;
        int npc = 0, beds = 0;
        const int max_npc = capital ? 220 : 150, max_beds = capital ? 80 : 50;
        for (StructurePoint& p : s.info.points) {
            if (p.kind == "npc" && p.tag != "guardia" && ++npc > max_npc && npc % 4 != 0) continue;
            if (p.kind == "cama" && ++beds > max_beds) continue;
            keep.push_back(std::move(p));
        }
        s.info.points = std::move(keep);
    }
    for (int g = 0; g < 4; ++g) s.hubs.push_back({outX[g], outZ[g]});
    finish(s);
    return true;
}

// --- Castillo en una colina (tipo "castillo") ----------------------------------------------------
// Meseta en lo alto, recinto exterior redondo de 8 lados con torres y puerta hacia la ladera mas
// suave, camino que sube, y dentro el castillo (homenaje con salon del trono, biblioteca, armeria,
// calabozos y camara del tesoro sellada), cuadras, cuartel con patio de armas, herreria y capilla.
bool tryHillCastle(Rng& xr) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const int lim = kHalf - 158;
        const int x = cx0 + xr.range(-lim, lim), z = cz0 + xr.range(-lim, lim);
        if (!clearSpot(x, z, 154.0f)) continue;  // (130 + 24: 30 m libres con lo demas, como las plazas hostiles)
        const SiteStats top = site(x, z, 40, 5);
        if (top.water > 0 || top.lava > 0 || top.max - top.min > 22 || top.min < sea + 3) continue;
        if (top.biome == Generator::AAshlands || top.biome == Generator::ABlight || top.biome == Generator::AOcean) continue;
        const SiteStats around = site(x, z, 105, 5);
        if (top.median - around.median < 4) continue;  // no es una colina
        buildHillCastle(x, z, top, xr);
        return true;
    }
    return false;
}

void buildHillCastle(int cx, int cz, const SiteStats& st, Rng& xr) {
    const BuildStyle bstyle = styleFor(st.biome) == BuildStyle::Beach ? BuildStyle::Temperate : styleFor(st.biome);
    const int E = 128;
    Hill hf = hillField(cx, cz, E, 4);
    const int top = std::max(roundDiv(hf.smooth16(cx, cz), 16) + 1, sea + 4);
    hf.base = top;
    Rng nr(hashOf(seed, rx, rz, 0xCA57));
    nr.next();
    Structure& s = begin("castillo", castleName(nr), cx, top + 1, cz, levelAt(cx, cz, 1.0f));
    s.clear_radius = 130.0f;
    s.seal = true;
    Terraform tf;
    tf.cx = cx;
    tf.cz = cz;
    tf.inner = 112.0f;
    tf.outer = 128.0f;
    tf.base = top;
    tf.surface = surfaceFor(bstyle);
    tf.under = bstyle == BuildStyle::Desert ? B::Sand : B::Dirt;
    tf.decor = 0.06f;
    s.terraforms.push_back(tf);
    s.ground_x0 = hf.x0;
    s.ground_z0 = hf.z0;
    s.ground_n = hf.n;
    s.ground.assign(static_cast<std::size_t>(hf.n) * static_cast<std::size_t>(hf.n), 0);
    s.ground_kind.assign(static_cast<std::size_t>(hf.n) * static_cast<std::size_t>(hf.n), Structure::GTerrain);
    const float fcx = static_cast<float>(cx) + 0.5f, fcz = static_cast<float>(cz) + 0.5f;
    constexpr float WR = 58.0f;
    for (int j = 0; j < hf.n; ++j)
        for (int i = 0; i < hf.n; ++i) {
            const int x = hf.x0 + i, z = hf.z0 + j;
            const float dx = static_cast<float>(x) + 0.5f - fcx, dz = static_cast<float>(z) + 0.5f - fcz;
            const float d2 = dx * dx + dz * dz;
            // Meseta llana dentro del recinto; fuera, la ladera en bancales.
            if (d2 <= (WR + 4.0f) * (WR + 4.0f)) groundPut(s, x, z, top, Structure::GPad);
            else groundPut(s, x, z, std::min(terraceAt(hf, x, z), top), Structure::GTerrain);
        }
    Occ occ;
    occ.init(hf.x0, hf.z0, hf.n);
    // Puerta hacia la ladera mas baja (de los 4 ejes).
    int kg = 0, low = 1 << 30;
    for (int k = 0; k < 16; k += 4) {
        const int h = hf.smooth16(static_cast<int>(fcx + kD16[k][0] * 100.0f), static_cast<int>(fcz + kD16[k][1] * 100.0f));
        if (h < low) {
            low = h;
            kg = k;
        }
    }
    // Camino de subida.
    {
        const float gx = fcx + kD16[kg][0] * WR, gz = fcz + kD16[kg][1] * WR;
        const float ox = fcx + kD16[kg][0] * 124.0f, oz = fcz + kD16[kg][1] * 124.0f;
        const Road r{gx, gz, ox, oz, 2.5f, B::Cobblestone, B::Gravel, 0.2f};
        groundRoad(s, r, profileBetween(hf, gx, gz, ox, oz, top, hillHeight(hf, ox, oz)));
        s.roads.push_back(r);
        occ.segment(gx, gz, ox, oz, 3.0f, Occ::Street, false);
        const Road in{fcx + kD16[kg][0] * 30.0f, fcz + kD16[kg][1] * 30.0f, gx, gz, 2.5f, B::Cobblestone, B::Gravel, 0.2f};
        s.roads.push_back(in);
        occ.segment(in.x0, in.z0, in.x1, in.z1, 3.0f, Occ::Street, false);
        s.hubs.push_back({ox, oz});
        hubs.push_back({ox, oz});
    }
    // Recinto exterior: 8 lados, torres en los vertices, puerta en kg.
    for (int k = 0; k < 16; k += 2) {
        const float ax = fcx + kD16[k][0] * WR, az = fcz + kD16[k][1] * WR;
        const int k2 = (k + 2) % 16;
        const float bx = fcx + kD16[k2][0] * WR, bz = fcz + kD16[k2][1] * WR;
        const float vx = bx - ax, vz = bz - az, len = std::sqrt(vx * vx + vz * vz);
        float a0x = ax, a0z = az, b0x = bx, b0z = bz;
        if (k == kg) a0x += vx / len * 7.5f, a0z += vz / len * 7.5f;
        if (k2 == kg) b0x -= vx / len * 7.5f, b0z -= vz / len * 7.5f;
        s.walls.push_back(WallLine{a0x, a0z, b0x, b0z, 10, 2.0f});
        occ.segment(a0x, a0z, b0x, b0z, 3.5f, Occ::Rampart, false);
        if (k == kg) {
            BuildContext ctx = context(BuildStyle::City, B::RoofSlate, 1100);
            build::gatehouse(ctx, 15, 14, 4);
            const int rot = rotFacing(kD16[k][0], kD16[k][1]);
            const int sx = (rot & 1) ? ctx.bp.d : ctx.bp.w, sz = (rot & 1) ? ctx.bp.w : ctx.bp.d;
            const int x0 = static_cast<int>(std::lround(ax - static_cast<float>(sx) * 0.5f)), z0 = static_cast<int>(std::lround(az - static_cast<float>(sz) * 0.5f));
            add(s, ctx, x0, top, z0, rot);
        } else {
            BuildContext ctx = context(BuildStyle::City, B::RoofSlate, 1200 + k);
            build::tower(ctx, 9, 17);
            const int size = ctx.bp.w;
            add(s, ctx, static_cast<int>(std::lround(ax)) - size / 2, top, static_cast<int>(std::lround(az)) - size / 2, rotFacing(fcx - ax, fcz - az));
            occ.fill(Rect{static_cast<int>(std::lround(ax)) - size / 2, static_cast<int>(std::lround(az)) - size / 2, static_cast<int>(std::lround(ax)) - size / 2 + size - 1,
                          static_cast<int>(std::lround(az)) - size / 2 + size - 1},
                     Occ::Rampart);
        }
    }
    // El castillo, al fondo y mirando a la puerta.
    {
        BuildContext ctx = context(BuildStyle::City, xr.f() < 0.5f ? B::RoofBlue : B::RoofSlate, 1300);
        build::castle(ctx, 51, 44, true);
        const float kx = fcx - kD16[kg][0] * 6.0f, kz = fcz - kD16[kg][1] * 6.0f;
        const int rot = rotFacing(kD16[kg][0], kD16[kg][1]);
        const int sx = (rot & 1) ? 44 : 51, sz = (rot & 1) ? 51 : 44;
        const int x0 = static_cast<int>(std::lround(kx - static_cast<float>(sx) * 0.5f)), z0 = static_cast<int>(std::lround(kz - static_cast<float>(sz) * 0.5f));
        add(s, ctx, x0, top - 6, z0, rot);
        occ.fill(Rect{x0 - 1, z0 - 1, x0 + sx, z0 + sz}, Occ::Building);
    }
    // Patio exterior: cuadras, cuartel, patio de armas, herreria, capilla, pozo y huerto.
    const int kinds[] = {MStable, MBarracks, MYard, MSmithy, MChapel, MGarden, MStable, MWorkCarpenter};
    for (int ki = 0; ki < 8; ++ki)
    for (int attempt = 0; attempt < 60; ++attempt) {
        const int kind = kinds[ki];
        int bw = 13, bd = 11;
        switch (kind) {
            case MBarracks: bw = 17; bd = 11; break;
            case MYard: bw = 15; bd = 11; break;
            case MSmithy: bw = 12; bd = 10; break;
            case MChapel: bw = 13; bd = 21; break;
            case MGarden: bw = 11; bd = 9; break;
            case MWorkCarpenter: bw = 12; bd = 10; break;
            default: break;
        }
        const int k = static_cast<int>(xr.next() % 16u);
        const float dist = 36.0f + xr.f() * 16.0f;
        const float x = fcx + kD16[k][0] * dist, z = fcz + kD16[k][1] * dist;
        BuildContext ctx = context(BuildStyle::City, B::RoofSlate, 2000 + attempt + ki * 64);
        const auto fn = [&](BuildContext& c) {
            switch (kind) {
                case MStable: build::stable(c); break;
                case MBarracks: build::barracks(c); break;
                case MYard: build::trainingYard(c, bw, bd); break;
                case MSmithy: build::smithy(c, true); break;
                case MChapel: c.roof = B::RoofBlue; build::temple(c); break;
                case MGarden: build::garden(c, bw, bd); break;
                default: build::workshop(c, "carpintero"); break;
            }
        };
        // Que quepa entero dentro del recinto.
        const float far = dist + static_cast<float>(std::max(bw, bd)) * 0.72f;
        if (far > WR - 3.5f) continue;
        if (metroPlace(s, occ, hf, ctx, fn, bw, bd, x, z, fcx - x, fcz - z, top, 0, 1, 99)) break;
    }
    {
        BuildContext wc = context(bstyle, B::RoofSlate, 2900);
        build::well(wc);
        const int wx = static_cast<int>(fcx + kD16[kg][0] * 40.0f + kD16[(kg + 4) % 16][0] * 10.0f);
        const int wz = static_cast<int>(fcz + kD16[kg][1] * 40.0f + kD16[(kg + 4) % 16][1] * 10.0f);
        if (occ.rectFree(Rect{wx - 2, wz - 2, wx + 2, wz + 2}, 0)) add(s, wc, wx - 2, top - 3, wz - 2, 0);
    }
    groundSteps(s);
    finish(s);
}

// --- Santuario misterioso (tipo "santuario") -----------------------------------------------------
std::string shrineName(Rng& r) {
    static const char* const kA[] = {"Shrine of the Hollow Moon", "Whispering Stones", "Chapel of the Drowned Saint", "Obelisk of the Ninth Star",
                                     "Circle of Ash", "Shrine of the Pale Lantern", "Stones of the Old Kings", "Chapel of Lost Bells",
                                     "Watcher's Obelisk", "Grove of Murmurs"};
    return r.pick(kA);
}

bool tryShrine(Rng& xr) {
    for (int attempt = 0; attempt < 6; ++attempt) {
        const int lim = kHalf - 47;
        const int x = cx0 + xr.range(-lim, lim), z = cz0 + xr.range(-lim, lim);
        if (!clearSpot(x, z, 43.0f)) continue;  // (19 + 24: 30 m libres con lo demas)
        const SiteStats st = site(x, z, 9, 3);
        if (st.water > 0 || st.lava > 0 || st.max - st.min > 4 || st.min < sea + 2) continue;
        Rng nr(hashOf(seed, rx, rz, 0x5A1));
        Structure& s = begin("santuario", shrineName(nr), x, st.median + 1, z, levelAt(x, z, 0.8f));
        pad(s, x, z, st.median, 9.0f, 15.0f, styleFor(st.biome));
        s.seal = true;
        BuildContext ctx = context(BuildStyle::Temperate, B::RoofSlate, 4300);
        build::shrine(ctx, static_cast<int>(xr.next() % 3u));
        add(s, ctx, x - ctx.bp.w / 2, st.median - 6, z - ctx.bp.d / 2, static_cast<int>(xr.next() & 3u));
        finish(s);
        return true;
    }
    return false;
}
