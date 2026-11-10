// Reino Profundo ("profundo", The Sunless Deep) y las simas abisales que bajan a el.
//
// SUPERFICIE (Aventura, carveSinkholes): lejos del origen (> ~1,1 km), como mucho una sima
// por celda de 640 x 640 m en tierra seca y sin estructuras cerca: un pozo redondo de
// 13-18 m de ancho desde el suelo hasta un estanque (agua en y 4..10, fondo de glifos que
// brillan), con anillos de piedra de glifos cada 14 m en las paredes, un brocal de piedra
// tallada y seis pilares runicos (uno con una tablilla). Atraviesa las cuevas inmensas
// (sus huecos se ven desde el pozo; solo se tapan los liquidos). Es una "estructura"
// (sima_abisal) con un punto "portal" (tag "profundo") en el estanque: el juego
// (Portales.cpp) cambia de dimension al caer al agua del fondo.
//
// REINO PROFUNDO (generateDeep): todo bajo techo (y 253..255 macizo: sin luz del cielo).
//   - Capa de abajo (suelo ~6..112, techo ~70..128): el mar sin sol (agua hasta y 39),
//     salas de goteo, selva de hongos luminosos, picos de cristal y llanuras de brasas
//     con rios de lava.
//   - Corteza (~28 m) con simas que la abren de lado a lado entre las dos capas.
//   - Capa de arriba (suelo ~150, techo ~236 con estalactitas): bosque de estalactitas
//     y columnas, selva de hongos gigantes y grutas de cristal.
//   - Arcos y rocas colgadas (ruido 3D), menas hondas (mitril, diamante, oro, cristal,
//     arcanita, hierro), enredaderas y raices luminosas colgando de los techos.
//   - La caida: bajo cada sima de la superficie, un pozo del techo (y 252) al lago de
//     abajo (cuenca de ~26 m de radio, agua de ~25 m) y la Piedra de Ascenso (portal a la
//     superficie) sobre un pilar del lago con su calzada.
//   - Por regiones de 256 m (deterministas): aldeas ocultas (aldea_oculta, amistosas: casas
//     seta, mercado, el Gran Sombrero, piedra de vuelta), ermitas, salas de los Delvers
//     (salon_enano, con sala secreta tras un muro agrietado), templos runicos (templo_runico,
//     puerta sellada que abre un acertijo de cuatro palancas de arcanita), santuarios del
//     culto (santuario_culto), colonias perdidas (colonia_perdida) y obeliscos con tablillas.
//     Los puntos (npc, mercader, spawner, jefe, cofre, inscripcion, puerta_runica,
//     lampara_runa, reliquia, portal, llegada) los usa el juego.
// Todo es funcion pura de (semilla, x, z): cualquier hilo, cualquier orden, sin cortes.
//
// CAPAS HONDAS (deep_layer_ 2..8: "profundo2".."profundo8", ~-250 m cada una hasta ~-2000 m): el mismo
// mundo de dos capas de cavernas con el tema de cada una (DeepTheme: biomas, mar de agua o de lava,
// roca, menas, columnas, guarniciones y jefe):
//   2 The Fungal Caverns   hongos gigantes por todas partes        5 The Delver Kingdom  la capital (palacio)
//   3 The Drowned Halls    mares subterraneos hasta y 70           6 The Magma Roots     mar y rios de lava
//   4 The Crystal Depths   cristal                                  7 The Hollow          huecos inmensos, roca palida
//   8 The Root Heart       raices vivas, lava y la guarida del Corazon de Raiz (jefe final)
// Pozos entre capas (layerShaft, rejilla de kShaftCell m, el 85 %): en la capa k un agujero en el suelo
// sobre una plataforma (sima_honda: estanque en y 2..9, punto "portal" a la capa k+1, "llegada" arriba en
// el borde) y en la k+1, en el mismo x, z, el pozo del techo al lago con la Escalera de los Delvers
// (espiral de escalones por la pared hasta y ~244: punto "portal" a la capa k) y la Piedra de Subida
// (portal a la capa k; "llegada" abajo, arriba del pozo). Desde la capa 3, ciudades (ciudad_profunda;
// caidas: ciudad_caida, hostiles) una por bloque de 3 x 3 regiones; en la 8, la guarida (corazon_raiz).

#include "VoxelInternal.h"
#include "Structures.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace cramion::voxel {

namespace {

constexpr float kPi = 3.14159265f;

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
int floorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
std::int64_t cellKey(int a, int b) { return (static_cast<std::int64_t>(a) << 32) ^ static_cast<std::uint32_t>(b); }
bool solidId(BlockId id) { return blockDef(id).solid && id != block::Lava; }

enum DeepKind : int { kNone = 0, kVillage, kHermit, kHall, kTemple, kSanctum, kColony, kObelisk, kAscension, kCity, kLair, kShaftHole };

// Lo propio de cada capa (indice 1..8; la 1 es el Reino Profundo de siempre, bloque a bloque).
struct DeepTheme {
    float ember_t, crystal_t, fungal_t;  // umbrales de los biomas de abajo (brasas, cristal, hongos)
    float up_crystal_t, up_fungal_t;     // y de los de arriba
    int sea;                             // nivel del liquido de la capa de abajo (0: sin mar)
    BlockId liquid;                      // agua o lava
    float floor_add;                     // el suelo de abajo, mas alto o mas bajo
    float stal;                          // estalactitas del techo de abajo (x)
    float chasm_t;                       // grietas en la corteza entre las dos capas
    BlockId rock;                        // la roca
    BlockId drip;                        // suelo de las salas de goteo
    BlockId column;                      // columnas del suelo al techo
    float ore;                           // riqueza de las menas (x)
    float arches;                        // umbral de los arcos y rocas colgadas (0: el de siempre)
    const char* species[3];              // guarniciones (ids del bestiario)
    const char* boss;                    // jefe de las plazas hostiles
};
const DeepTheme kThemes[Generator::kDeepLayers + 1] = {
    {0.14f, 0.14f, 0.0f, 0.12f, 0.05f, 40, block::Water, 0.0f, 1.0f, 0.009f, block::UmbralStone, block::Dripstone, block::Dripstone, 1.0f, 0.0f,
     {"golem", "esqueleto", "arana"}, "golem"},
    // 1 The Sunless Deep (el de siempre)
    {0.14f, 0.14f, 0.0f, 0.12f, 0.05f, 40, block::Water, 0.0f, 1.0f, 0.009f, block::UmbralStone, block::Dripstone, block::Dripstone, 1.0f, 0.0f,
     {"golem", "esqueleto", "arana"}, "golem"},
    // 2 The Fungal Caverns: casi todo selva de hongos, arriba y abajo
    {0.36f, 0.36f, -0.45f, 0.40f, -0.35f, 32, block::Water, 0.0f, 0.8f, 0.009f, block::UmbralStone, block::Dripstone, block::ShroomStem, 1.15f,
     0.0f, {"hongo", "arana", "troll_cueva"}, "troll_cueva"},
    // 3 The Drowned Halls: mares subterraneos (agua hasta y 70)
    {0.30f, 0.18f, 0.05f, 0.12f, 0.10f, 70, block::Water, -6.0f, 1.0f, 0.012f, block::UmbralStone, block::Dripstone, block::Dripstone, 1.3f,
     0.0f, {"rana", "lagarto", "cocodrilo"}, "hidra"},
    // 4 The Crystal Depths: cristal
    {0.36f, -0.25f, 0.30f, -0.30f, 0.30f, 36, block::Water, 0.0f, 0.9f, 0.009f, block::UmbralStone, block::DuskStone, block::Dripstone, 1.45f,
     0.0f, {"escarabajo", "golem", "elemental_hielo"}, "golem"},
    // 5 The Delver Kingdom: salas anchas y llanas (sitio para ciudades)
    {0.26f, 0.20f, 0.05f, 0.15f, 0.10f, 38, block::Water, 4.0f, 0.7f, 0.006f, block::UmbralStone, block::Dripstone, block::Dripstone, 1.6f, 0.05f,
     {"caballero_espectral", "golem", "esqueleto"}, "caudillo_orco"},
    // 6 The Magma Roots: brasas, rios y mar de lava
    {-0.35f, 0.30f, 0.40f, 0.25f, 0.30f, 50, block::Lava, 0.0f, 1.0f, 0.012f, block::Basalt, block::Basalt, block::Basalt, 1.8f, 0.0f,
     {"salamandra", "diablillo", "golem"}, "gigante"},
    // 7 The Hollow: suelo hundido, huecos inmensos, roca palida y rocas colgadas
    {0.36f, 0.05f, 0.35f, 0.00f, 0.40f, 0, block::Air, -24.0f, 1.4f, 0.035f, block::HollowStone, block::HollowStone, block::HollowStone, 2.0f,
     -0.08f, {"espectro", "caballero_espectral", "nigromante"}, "rey_liche"},
    // 8 The Root Heart: raices vivas, lava abajo
    {0.05f, 0.10f, 0.40f, -0.10f, 0.40f, 34, block::Lava, -8.0f, 1.0f, 0.015f, block::UmbralStone, block::Dripstone, block::HeartRoot, 2.4f,
     0.0f, {"espectro", "golem", "troll_cueva"}, "rey_liche"},
};
const DeepTheme& themeOf(int layer) { return kThemes[std::clamp(layer, 1, Generator::kDeepLayers)]; }
// Nombre de la dimension de una capa (1 -> "profundo").
std::string layerDim(int layer) { return layer <= 1 ? std::string("profundo") : "profundo" + std::to_string(std::min(layer, Generator::kDeepLayers)); }
// Desplazamiento del ruido de cada capa (la 1, ninguno: el mundo de siempre).
float layerOffX(int layer) { return layer > 1 ? static_cast<float>(layer - 1) * 9973.0f : 0.0f; }
float layerOffZ(int layer) { return layer > 1 ? static_cast<float>(layer - 1) * -6113.0f : 0.0f; }
// Tablillas de cada capa honda: 13 + 2 (capa - 2) y la siguiente (la 27, el final, en la guarida).
int layerLore(int layer) { return 13 + 2 * (std::clamp(layer, 2, Generator::kDeepLayers) - 2); }

// Escribe una pieza en coordenadas locales (girada rot * 90 grados alrededor del ancla) en el
// chunk (lo que cae dentro) o apunta sus puntos (planificacion).
struct Sink {
    ChunkData* chunk = nullptr;
    std::vector<StructurePoint>* pts = nullptr;
    int ax = 0, ay = 0, az = 0, rot = 0;
    std::uint32_t seed = 0;

    void toWorld(int lx, int lz, int& wx, int& wz) const {
        switch (rot & 3) {
            case 0: wx = ax + lx; wz = az + lz; break;
            case 1: wx = ax - lz; wz = az + lx; break;
            case 2: wx = ax - lx; wz = az - lz; break;
            default: wx = ax + lz; wz = az - lx; break;
        }
    }
    BlockId* cell(int lx, int ly, int lz) const {
        if (chunk == nullptr) return nullptr;
        int wx = 0, wz = 0;
        toWorld(lx, lz, wx, wz);
        const int y = ay + ly;
        const int cx = wx - chunk->cx * kChunkSize, cz = wz - chunk->cz * kChunkSize;
        if (cx < 0 || cz < 0 || cx >= kChunkSize || cz >= kChunkSize || y < 1 || y >= kWorldHeight - 1) return nullptr;
        return &chunk->blocks[static_cast<std::size_t>(cellIndex(cx, y, cz))];
    }
    void set(int lx, int ly, int lz, BlockId id) const {
        if (BlockId* c = cell(lx, ly, lz)) *c = id;
    }
    // Solo sobre aire o algo que se reemplaza (plantas, agua).
    void soft(int lx, int ly, int lz, BlockId id) const {
        if (BlockId* c = cell(lx, ly, lz)) {
            if (*c == block::Air || blockDef(*c).replaceable) *c = id;
        }
    }
    // Relleno de cimientos: solo donde no hay suelo.
    void fill(int lx, int ly, int lz, BlockId id) const {
        if (BlockId* c = cell(lx, ly, lz)) {
            if (!solidId(*c)) *c = id;
        }
    }
    void box(int x0, int y0, int z0, int x1, int y1, int z1, BlockId id) const {
        if (chunk == nullptr) return;
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) set(x, y, z, id);
    }
    float rnd(int a, int b = 0, int c = 0) const { return rand01(a, b * 131 + c, c * 7 - b, seed); }
    // Punto del juego en el centro de la celda (dir local: 0 -z, 1 +x, 2 +z, 3 -x).
    void point(const char* kind, int lx, int ly, int lz, int dir, const std::string& tag = {}) const {
        if (pts == nullptr) return;
        int wx = 0, wz = 0;
        toWorld(lx, lz, wx, wz);
        static constexpr int kDx[4] = {0, 1, 0, -1}, kDz[4] = {-1, 0, 1, 0};
        const int dx = kDx[dir & 3], dz = kDz[dir & 3];
        int vx = dx, vz = dz;
        switch (rot & 3) {
            case 1: vx = -dz; vz = dx; break;
            case 2: vx = -dx; vz = -dz; break;
            case 3: vx = dz; vz = -dx; break;
            default: break;
        }
        StructurePoint p;
        p.kind = kind;
        p.position = core::Vec3{static_cast<float>(wx) + 0.5f, static_cast<float>(ay + ly), static_cast<float>(wz) + 0.5f};
        p.direction = core::Vec3{static_cast<float>(vx), 0.0f, static_cast<float>(vz)};
        p.tag = tag;
        pts->push_back(std::move(p));
    }
};

// Direccion local (0..3) que mas se parece al vector (x, z) (para mirar a un sitio).
int dirToward(float x, float z) {
    if (std::abs(x) > std::abs(z)) return x > 0.0f ? 1 : 3;
    return z > 0.0f ? 2 : 0;
}

void lampPost(const Sink& s, int x, int z) {
    s.set(x, 0, z, block::LampPost);
    s.set(x, 1, z, block::LampPost);
    s.set(x, 2, z, block::LampPost);
    s.set(x, 3, z, block::StreetLamp);
}

// Casa seta de los Lanternfolk (radio ~3, sombrero de 4.6): puerta hacia (tx, tz).
void mushroomHouse(const Sink& s, int hx, int hz, int tx, int tz, BlockId cap, const char* npc_tag, int index) {
    const float vx = static_cast<float>(tx - hx), vz = static_cast<float>(tz - hz);
    const float vl = std::max(0.01f, std::hypot(vx, vz));
    const float ux = vx / vl, uz = vz / vl;
    const int door_x = hx + static_cast<int>(std::lround(ux * 3.0f)), door_z = hz + static_cast<int>(std::lround(uz * 3.0f));
    for (int dz = -5; dz <= 5; ++dz) {
        for (int dx = -5; dx <= 5; ++dx) {
            const float d = std::hypot(static_cast<float>(dx), static_cast<float>(dz));
            const int x = hx + dx, z = hz + dz;
            if (d <= 3.3f) {
                s.set(x, -1, z, d <= 2.3f ? block::SprucePlanks : block::ShroomStem);
                for (int y = 0; y <= 3; ++y) s.set(x, y, z, d > 2.3f ? block::ShroomStem : block::Air);
            }
            // Sombrero: tapa y cupula con el borde caido.
            for (int dy = 0; dy <= 3; ++dy) {
                const float k = static_cast<float>(dy) / 3.8f;
                const float rr = 4.6f * std::sqrt(std::max(0.0f, 1.0f - k * k));
                if (d <= rr && (dy == 0 || d > rr - 1.4f || dy == 3)) s.set(x, 4 + dy, z, cap);
            }
            if (d > 3.6f && d <= 4.6f) s.soft(x, 3, z, cap);  // ala
        }
    }
    s.set(door_x, 0, door_z, block::Door);
    s.set(door_x, 1, door_z, block::Door);
    s.set(door_x - static_cast<int>(std::lround(ux)), -1, door_z - static_cast<int>(std::lround(uz)), block::SprucePlanks);
    // Ventanas de cristal a los lados.
    const int px = static_cast<int>(std::lround(-uz * 3.0f)), pz = static_cast<int>(std::lround(ux * 3.0f));
    s.set(hx + px, 2, hz + pz, block::Glass);
    s.set(hx - px, 2, hz - pz, block::Glass);
    // Dentro: farol colgado, cama al fondo, mesa, estanteria o barril.
    s.set(hx, 3, hz, block::Lantern);
    const int bx = hx - static_cast<int>(std::lround(ux * 1.7f)), bz = hz - static_cast<int>(std::lround(uz * 1.7f));
    s.set(bx, 0, bz, block::Bed);
    s.set(hx + static_cast<int>(std::lround(-uz * 1.6f)), 0, hz + static_cast<int>(std::lround(ux * 1.6f)), block::Table);
    s.set(hx - static_cast<int>(std::lround(-uz * 1.6f)), 0, hz - static_cast<int>(std::lround(ux * 1.6f)),
          index % 2 ? block::Barrel : block::Bookshelf);
    const int dir = dirToward(ux, uz);
    s.point("npc", hx, 0, hz, dir, npc_tag);
    s.point("cama", bx, 0, bz, dir);
    s.point("entrada", door_x + static_cast<int>(std::lround(ux)), 0, door_z + static_cast<int>(std::lround(uz)), dir);
}

// Gran Sombrero de la plaza: tallo de 2 x 2 y sombrero luminoso de radio 6.5 con farolillos.
void elderCap(const Sink& s) {
    for (int y = 0; y <= 8; ++y)
        for (int z = 0; z <= 1; ++z)
            for (int x = 0; x <= 1; ++x) s.set(x, y, z, block::ShroomStem);
    const float radius = 6.5f, cap_h = 3.6f, inner = radius - 1.6f;
    for (int dy = -1; dy <= 4; ++dy) {
        for (int dz = -7; dz <= 8; ++dz) {
            for (int dx = -7; dx <= 8; ++dx) {
                const float fx = static_cast<float>(dx) - 0.5f, fz = static_cast<float>(dz) - 0.5f;
                const float r2 = fx * fx + fz * fz;
                if (dy < 0) {
                    const float k = r2 / (radius * radius);
                    if (k >= 0.72f && k <= 1.0f) s.set(dx, 9 + dy, dz, block::ShroomCapBlue);
                    continue;
                }
                const float outer_e = r2 / (radius * radius) + static_cast<float>(dy * dy) / (cap_h * cap_h);
                const float in_y = static_cast<float>(dy) + 1.2f;
                const float inner_e = r2 / (inner * inner) + in_y * in_y / (cap_h * cap_h);
                if (outer_e <= 1.0f && inner_e > 1.0f) s.set(dx, 9 + dy, dz, block::ShroomCapBlue);
            }
        }
    }
    for (int i = 0; i < 6; ++i) {
        const float a = static_cast<float>(i) * kPi / 3.0f;
        s.set(static_cast<int>(std::lround(0.5f + std::cos(a) * 4.5f)), 7, static_cast<int>(std::lround(0.5f + std::sin(a) * 4.5f)),
              block::PaperLantern);
    }
    s.set(3, 0, 0, block::Bench);
    s.set(-2, 0, 1, block::Bench);
    s.set(0, 0, 3, block::Bench);
    s.set(1, 0, -2, block::Bench);
}

void buildVillage(const Sink& s) {
    // Suelo: plaza empedrada, calles en cruz de tierra pisada.
    for (int z = -22; z <= 22; ++z) {
        for (int x = -22; x <= 22; ++x) {
            const float d = std::hypot(static_cast<float>(x), static_cast<float>(z));
            if (d <= 5.5f) s.set(x, -1, z, block::Paving);
            else if ((std::abs(x) <= 1 || std::abs(z) <= 1) && d <= 22.0f) s.set(x, -1, z, s.rnd(x, z) < 0.2f ? block::Gravel : block::DirtPath);
        }
    }
    elderCap(s);
    s.point("plaza", 0, 0, -4, 0);
    // Casas seta alrededor (5) mirando a la plaza.
    static const char* const kTags[5] = {"anciano", "herrero", "tabernero", "mago", "granjero"};
    for (int i = 0; i < 5; ++i) {
        const float a = (22.0f + 72.0f * static_cast<float>(i)) * kPi / 180.0f + (s.rnd(i, 3) - 0.5f) * 0.25f;
        const float r = 14.5f + s.rnd(i, 4) * 2.0f;
        const int hx = static_cast<int>(std::lround(std::cos(a) * r)), hz = static_cast<int>(std::lround(std::sin(a) * r));
        mushroomHouse(s, hx, hz, 0, 0, i % 2 ? block::ShroomCapViolet : block::ShroomCapBlue, kTags[i], i);
    }
    // Mercado: dos puestos con toldo.
    const auto stall = [&](int sx, int sz, BlockId cloth, const char* shop) {
        s.set(sx, 0, sz, block::Counter);
        s.set(sx + 1, 0, sz, block::Counter);
        for (const int px : {sx - 1, sx + 2})
            for (const int pz : {sz - 1, sz + 1})
                for (int y = 0; y <= 2; ++y) s.set(px, y, pz, block::Fence);
        s.box(sx - 1, 3, sz - 1, sx + 2, 3, sz + 1, cloth);
        s.set(sx + 1, 1, sz, block::Lantern);
        s.point("mercader", sx, 0, sz + 1, 0, shop);
    };
    stall(5, -8, block::ClothBlue, "profunda");
    stall(-7, -8, block::ClothYellow, "pociones");
    // Farolas por las calles.
    for (const int r : {8, 18}) {
        lampPost(s, 2, r);
        lampPost(s, -2, -r);
        lampPost(s, r, -2);
        lampPost(s, -r, 2);
    }
    // Huerto de setas y algunos barriles y cajas.
    for (int z = 8; z <= 11; ++z)
        for (int x = -12; x <= -8; ++x) {
            s.set(x, -1, z, block::DuskSoil);
            const float r = s.rnd(x, z, 5);
            s.set(x, 0, z, r < 0.35f ? block::GlowMushroom : (r < 0.6f ? block::BrownMushroom : (r < 0.75f ? block::RedMushroom : block::Air)));
        }
    s.set(9, 0, 9, block::Barrel);
    s.set(10, 0, 9, block::Crate);
    s.set(-9, 0, -12, block::Crate);
    // Piedra de vuelta (portal a la superficie) y la tablilla de bienvenida.
    for (int z = -14; z <= -10; ++z)
        for (int x = -2; x <= 2; ++x)
            if (x * x + (z + 12) * (z + 12) <= 5) s.set(x, -1, z, block::DelverBricks);
    for (const int x : {-2, 2}) {
        for (int y = 0; y <= 2; ++y) s.set(x, y, -12, block::RuneStone);
        s.set(x, 3, -12, block::GlyphStone);
    }
    s.point("portal", 0, 0, -12, 0, "superficie");
    s.set(4, 0, -12, block::LoreTablet);
    s.point("inscripcion", 4, 0, -11, 0, "aldea");
    static constexpr int kGates[4][2] = {{0, -22}, {0, 22}, {22, 0}, {-22, 0}};
    for (const auto& g : kGates) s.point("entrada", g[0], 0, g[1], 0);
}

void buildHermit(const Sink& s, int lore) {
    s.box(-3, -1, -2, 3, -1, 2, block::DarkPlanks);
    for (int y = 0; y <= 3; ++y)
        for (int z = -2; z <= 2; ++z)
            for (int x = -3; x <= 3; ++x) {
                const bool wall = std::abs(x) == 3 || std::abs(z) == 2;
                s.set(x, y, z, wall ? block::CarvedStone : block::Air);
            }
    s.box(-4, 4, -3, 4, 4, 3, block::RoofSlate);
    s.box(-2, 5, -1, 2, 5, 1, block::RoofSlate);
    s.set(0, 0, -2, block::Door);
    s.set(0, 1, -2, block::Door);
    s.set(-3, 2, 0, block::Glass);
    s.set(3, 2, 0, block::Glass);
    s.set(0, 3, 0, block::Lantern);
    s.set(2, 0, 1, block::Bed);
    s.set(-2, 0, 1, block::Bookshelf);
    s.set(-2, 1, 1, block::Bookshelf);
    s.set(-2, 0, -1, block::Table);
    s.set(-2, 1, -1, block::Candles);
    s.set(0, 1, 2, block::LoreTablet);  // en la pared del fondo
    s.set(0, 0, -5, block::Campfire);
    s.set(2, 0, -5, block::Bench);
    for (int i = 0; i < 10; ++i) {
        const int x = -6 + static_cast<int>(s.rnd(i, 1) * 13.0f), z = -6 + static_cast<int>(s.rnd(i, 2) * 13.0f);
        if (std::abs(x) <= 4 && std::abs(z) <= 3) continue;
        s.soft(x, 0, z, i % 3 ? block::GlowMushroom : block::BrownMushroom);
    }
    s.point("mercader", 0, 0, 0, 0, "profunda");
    s.point("ermita", 0, 0, 0, 0);
    s.point("cama", 2, 0, 1, 0);
    s.point("inscripcion", 0, 0, 1, 2, std::to_string(lore));
    s.point("entrada", 0, 0, -3, 0);
}

void buildHall(const Sink& s, int lore) {
    // Suelo, muros en ruinas, columnas con glifos, techo roto.
    s.box(-15, -1, -11, 15, -1, 11, block::DelverBricks);
    for (int y = 0; y <= 9; ++y)
        for (int z = -11; z <= 11; ++z)
            for (int x = -15; x <= 15; ++x) {
                const bool wall = std::abs(x) == 15 || std::abs(z) == 11;
                if (!wall) continue;
                if (z == -11 && std::abs(x) <= 2 && y <= 4) continue;  // entrada
                if (x == 15 && std::abs(z) <= 3) {                         // muro de la sala secreta: entero
                    s.set(x, y, z, std::abs(z) <= 1 && y <= 2 ? block::CrackedDelverBricks : block::DelverBricks);
                    continue;
                }
                if (y > 2 && s.rnd(x, y, z) < 0.16f + static_cast<float>(y) * 0.02f) continue;  // derrumbes
                s.set(x, y, z, y == 5 && (x + z) % 4 == 0 ? block::GlyphStone : block::DelverBricks);
            }
    for (int z = -10; z <= 10; ++z)
        for (int x = -14; x <= 14; ++x)
            if (s.rnd(x, 40, z) > 0.4f) s.set(x, 10, z, block::DelverBricks);
    for (const int cx : {-9, -3, 3, 9})
        for (const int cz : {-5, 5})
            for (int y = 0; y <= 9; ++y)
                for (int dz = 0; dz <= 1; ++dz)
                    for (int dx = 0; dx <= 1; ++dx) {
                        if (y > 6 && s.rnd(cx, y, cz) < 0.3f) continue;
                        s.set(cx + dx, y, cz + dz, y == 6 ? block::GlyphStone : block::DelverBricks);
                    }
    // Estrado del trono al fondo, estandartes, braseros y escombros.
    s.box(-3, 0, 8, 3, 0, 10, block::DelverBricks);
    s.box(-2, 1, 9, 2, 1, 10, block::DelverBricks);
    s.set(0, 2, 10, block::Throne);
    s.set(0, 3, 11, block::LoreTablet);
    for (const int x : {-5, 5}) {
        s.set(x, 3, 11, block::BannerBlue);
        s.set(x, 4, 11, block::BannerBlue);
    }
    for (const int x : {-12, 12})
        for (const int z : {-8, 8}) s.set(x, 0, z, block::SoulBrazier);
    for (int i = 0; i < 26; ++i) {
        const int x = -13 + static_cast<int>(s.rnd(i, 50) * 27.0f), z = -9 + static_cast<int>(s.rnd(i, 51) * 17.0f);
        const float r = s.rnd(i, 52);
        s.soft(x, 0, z, r < 0.3f ? block::Bones : (r < 0.55f ? block::Cobweb : (r < 0.75f ? block::DelverBricks : block::Bookshelf)));
    }
    // Sala secreta tras el muro agrietado (x 16..20): cascara de ladrillos que no se rompen.
    s.box(16, -1, -3, 21, 4, 3, block::DelverBricks);
    s.box(16, 0, -2, 20, 3, 2, block::Air);
    s.set(18, 4, 0, block::GlyphStone);
    s.set(21, 1, 0, block::LoreTablet);
    s.set(20, 0, -2, block::GoldBlock);
    s.set(20, 0, 2, block::GoldBlock);
    s.point("cofre", 19, 0, 0, 3, "4");
    s.point("inscripcion", 19, 0, 1, 1, std::to_string(lore + 1));
    s.point("secreto", 16, 0, 0, 1);
    // Guardianes, jefe, cofres, tablilla del trono.
    s.point("spawner", -6, 0, 0, 2, "golem");
    s.point("spawner", 6, 0, -4, 0, "esqueleto");
    s.point("spawner", 0, 0, 4, 0, "caballero_espectral");
    s.point("spawner", -10, 0, 6, 1, "arana");
    s.point("jefe", 0, 1, 8, 0, "golem");
    s.point("cofre", -12, 0, 6, 2, "2");
    s.point("cofre", 12, 0, 6, 2, "3");
    s.point("inscripcion", 0, 1, 8, 2, std::to_string(lore));
    s.point("entrada", 0, 0, -12, 0);
}

// Templo runico: puerta de la camara sellada que se abre con el acertijo de las cuatro llamas.
// `pattern` (4 bits, 1..14): que palancas tienen que estar encendidas; `colors`: el color de la
// lampara de cada palanca (0 roja, 1 azul, 2 verde, 3 blanca).
void buildTemple(const Sink& s, int pattern, const int colors[4]) {
    s.box(-10, -1, -13, 10, -1, 13, block::DelverBricks);
    for (int y = 0; y <= 9; ++y)
        for (int z = -13; z <= 13; ++z)
            for (int x = -10; x <= 10; ++x) {
                const bool wall = std::abs(x) == 10 || std::abs(z) == 13 || z == 5;
                if (!wall || (z == -13 && std::abs(x) <= 1 && y <= 3)) {  // dentro o la entrada
                    s.set(x, y, z, block::Air);
                    continue;
                }
                s.set(x, y, z, y == 8 && std::abs(x) == 10 && z % 3 == 0 ? block::GlyphStone : block::DelverBricks);
            }
    s.box(-10, 10, -13, 10, 10, 13, block::DelverBricks);
    for (int z = -11; z <= 11; z += 4)
        for (const int x : {-6, 0, 6}) s.set(x, 10, z, block::GlyphStone);
    for (const int x : {-4, 4}) {  // columnas de la fachada
        for (int y = 0; y <= 7; ++y) s.set(x, y, -15, block::DelverBricks);
        s.set(x, 8, -15, block::GlyphStone);
    }
    s.box(-5, -1, -16, 5, -1, -14, block::DelverBricks);
    // Las cuatro palancas: lampara de color delante, palanca, dos de polvo de arcanita y la
    // lampara de senal en el muro de la camara.
    static constexpr int kX[4] = {-7, -4, 4, 7};
    static constexpr BlockId kColor[4] = {block::LampRed, block::LampBlue, block::LampGreen, block::CrystalLamp};
    std::string bits;
    for (int i = 0; i < 4; ++i) {
        const int x = kX[i];
        s.set(x, 0, 1, kColor[colors[i] & 3]);
        s.set(x, 0, 2, block::RuneLever);
        s.set(x, 0, 3, block::ArcaniteTrace);
        s.set(x, 0, 4, block::ArcaniteTrace);
        s.set(x, 0, 5, block::SignalLamp);
        s.point("lampara_runa", x, 0, 5, 0, std::to_string(i) + ":" + std::to_string((pattern >> i) & 1));
        bits += ((pattern >> i) & 1) ? '1' : '0';
    }
    // Puerta sellada (3 x 4) y la camara: altar con la reliquia, cofres y oro.
    s.point("puerta_runica", 0, 0, 5, 0, bits);
    std::string tag = "acertijo:";
    for (int i = 0; i < 4; ++i) tag += std::to_string(colors[i] & 3) + std::to_string((pattern >> i) & 1);
    s.set(0, 0, -4, block::LoreTablet);
    s.point("inscripcion", 0, 0, -5, 2, tag);
    s.set(0, 0, 11, block::Altar);
    s.point("reliquia", 0, 0, 10, 0, "corazon");
    s.point("cofre", -5, 0, 9, 1, "4");
    s.point("cofre", 5, 0, 9, 3, "3");
    for (const int x : {-8, -7, 7, 8}) s.set(x, 0, 12, block::GoldBlock);
    s.set(-8, 1, 12, block::GoldBlock);
    s.set(0, 9, 9, block::GlyphStone);
    // Guardianes.
    s.point("spawner", -6, 0, -7, 2, "golem");
    s.point("spawner", 6, 0, -7, 2, "golem");
    s.point("spawner", 0, 0, -10, 2, "escarabajo");
    s.point("entrada", 0, 0, -14, 0);
}

void buildSanctum(const Sink& s, int lore) {
    for (int z = -14; z <= 14; ++z)
        for (int x = -14; x <= 14; ++x) {
            const float d = std::hypot(static_cast<float>(x), static_cast<float>(z));
            if (d <= 13.5f) s.set(x, -1, z, d < 4.5f ? block::CrackedBlackstone : block::BlackstoneBricks);
            if (d > 13.0f && d <= 14.2f && s.rnd(x, z, 3) > 0.25f) {
                s.set(x, 0, z, block::BlackstoneBricks);
                if (s.rnd(x, z, 4) > 0.5f) s.set(x, 1, z, block::BlackstoneBricks);
            }
        }
    for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * kPi / 4.0f;
        const int x = static_cast<int>(std::lround(std::cos(a) * 10.5f)), z = static_cast<int>(std::lround(std::sin(a) * 10.5f));
        const int h = 4 + static_cast<int>(s.rnd(i, 7) * 3.0f);
        for (int y = 0; y < h; ++y) s.set(x, y, z, block::BlackstoneBricks);
        s.set(x, h, z, block::SoulBrazier);
        s.set(x, h - 2, z, block::BannerPurple);
    }
    s.box(-1, 0, -1, 1, 0, 1, block::BlackstoneBricks);
    s.set(0, 1, 0, block::Altar);
    s.set(-1, 1, -1, block::Candles);
    s.set(1, 1, 1, block::Candles);
    s.set(1, 1, -1, block::SkullPile);
    s.set(0, 0, -3, block::LoreTablet);
    for (int i = 0; i < 18; ++i) {
        const int x = -11 + static_cast<int>(s.rnd(i, 60) * 23.0f), z = -11 + static_cast<int>(s.rnd(i, 61) * 23.0f);
        if (std::abs(x) <= 2 && std::abs(z) <= 3) continue;
        s.soft(x, 0, z, s.rnd(i, 62) < 0.5f ? block::Bones : block::Candles);
    }
    s.point("spawner", 5, 0, 5, 0, "brujo");
    s.point("spawner", -5, 0, 5, 0, "brujo");
    s.point("spawner", 5, 0, -6, 2, "espectro");
    s.point("spawner", -5, 0, -6, 2, "espectro");
    s.point("spawner", 0, 0, 8, 0, "esqueleto");
    s.point("jefe", 0, 0, 3, 0, "nigromante");
    s.point("cofre", 3, 0, 0, 3, "3");
    s.point("inscripcion", 0, 0, -4, 0, std::to_string(lore));
    s.point("entrada", 0, 0, -14, 0);
}

void buildColony(const Sink& s, int lore) {
    for (int z = -17; z <= 17; ++z)
        for (int x = -17; x <= 17; ++x) {
            const float d = std::hypot(static_cast<float>(x), static_cast<float>(z));
            if (d <= 17.0f) s.set(x, -1, z, s.rnd(x, z) < 0.35f ? block::Gravel : block::DirtPath);
            if (d > 15.5f && d <= 16.5f && s.rnd(x, z, 9) > 0.45f) {  // empalizada caida a trozos
                const int h = 1 + static_cast<int>(s.rnd(x, z, 8) * 3.0f);
                for (int y = 0; y < h; ++y) s.set(x, y, z, block::Palisade);
            }
        }
    // Cuatro casas en ruinas.
    for (int i = 0; i < 4; ++i) {
        const int hx = (i & 1) ? 9 : -9, hz = (i & 2) ? 9 : -9;
        for (int y = 0; y <= 3; ++y)
            for (int z = -3; z <= 3; ++z)
                for (int x = -3; x <= 3; ++x) {
                    const bool wall = std::abs(x) == 3 || std::abs(z) == 3;
                    if (!wall) continue;
                    if (z == (hz > 0 ? -3 : 3) && x == 0 && y <= 1) continue;  // puerta hacia el centro
                    if (y > 0 && s.rnd(hx + x, y, hz + z) < 0.3f) continue;
                    s.set(hx + x, y, hz + z, (x + z + y) % 3 == 0 ? block::Timber : block::SprucePlanks);
                }
        if (i % 2 == 0)
            for (int z = -3; z <= 3; ++z)
                for (int x = -3; x <= 3; ++x)
                    if (s.rnd(hx + x, 9, hz + z) < 0.5f) s.set(hx + x, 4, hz + z, block::Thatch);
        s.set(hx + 2, 0, hz + 2, block::Cobweb);
        s.set(hx - 2, 0, hz + 1, i % 2 ? block::Crate : block::Barrel);
        s.set(hx + 1, 0, hz - 2, block::Bones);
        s.set(hx - 2, 2, hz - 2, block::Cobweb);
    }
    // Vagonetas abandonadas: una via de lado a lado con topes, cajas y la boca de una mina.
    for (int x = -13; x <= 13; ++x) s.set(x, 0, 0, std::abs(x) == 13 ? block::BufferStop : block::Rail);
    for (int y = 0; y <= 4; ++y) {
        s.set(-15, y, -2, block::Timber);
        s.set(-15, y, 2, block::Timber);
    }
    s.box(-15, 5, -2, -15, 5, 2, block::Timber);
    s.set(4, 0, 2, block::Crate);
    s.set(5, 0, 2, block::Crate);
    s.set(4, 1, 2, block::Crate);
    s.set(-9, 0, -7, block::Table);
    s.set(-9, 1, -7, block::LoreTablet);
    s.point("spawner", 9, 0, -9, 0, "arana");
    s.point("spawner", -9, 0, 9, 0, "arana");
    s.point("spawner", -9, 0, -11, 0, "zombi");
    s.point("spawner", 9, 0, 9, 0, "esqueleto");
    s.point("jefe", 0, 0, 5, 0, "troll_cueva");
    s.point("cofre", 9, 0, 8, 0, "2");
    s.point("cofre", -10, 0, 10, 0, "2");
    s.point("inscripcion", -9, 0, -6, 0, std::to_string(lore));
    s.point("entrada", 0, 0, -17, 0);
}

void buildObelisk(const Sink& s, int lore) {
    for (int z = -3; z <= 3; ++z)
        for (int x = -3; x <= 3; ++x)
            if (x * x + z * z <= 11) s.set(x, -1, z, block::DelverBricks);
    s.box(-1, 0, -1, 1, 6, 1, block::DelverBricks);
    s.box(-1, 4, -1, 1, 4, 1, block::GlyphStone);
    s.box(0, 7, 0, 0, 9, 0, block::DelverBricks);
    s.set(0, 10, 0, block::GlyphStone);
    s.set(0, 1, -1, block::LoreTablet);
    for (const int x : {-2, 2}) s.set(x, 0, -3, block::Candles);
    s.point("inscripcion", 0, 0, -3, 2, std::to_string(lore));
}

// Piedra de Ascenso: dais sobre un pilar que sale del lago de la caida (local y 0 = encima del
// dais; el dais en y -1) y calzada hacia fuera (+z). `tag`: a donde lleva ("superficie" o la capa de
// arriba: Piedra de Subida); `depth`: lo que baja el pilar.
void buildAscension(const Sink& s, const std::string& tag, int depth) {
    for (int z = -4; z <= 4; ++z)
        for (int x = -4; x <= 4; ++x) {
            const float d = std::hypot(static_cast<float>(x), static_cast<float>(z));
            if (d > 3.4f) continue;
            for (int y = -depth; y <= -2; ++y) s.set(x, y, z, block::DelverBricks);
            s.set(x, -1, z, d > 2.4f ? block::GlyphStone : block::DelverBricks);
            for (int y = 0; y <= 5; ++y) s.set(x, y, z, block::Air);
        }
    for (const int x : {-2, 2}) {
        for (int y = 0; y <= 2; ++y) s.set(x, y, 0, block::RuneStone);
        s.set(x, 3, 0, block::GlyphStone);
    }
    for (int z = 3; z <= 20; ++z) {
        for (int x = -1; x <= 1; ++x) {
            s.set(x, -1, z, std::abs(x) == 1 && z % 4 == 2 ? block::GlyphStone : block::DelverBricks);
            for (int y = 0; y <= 4; ++y) s.set(x, y, z, block::Air);
            if (z % 5 == 0 && x != 0)
                for (int y = -depth; y <= -2; ++y) s.fill(x, y, z, block::DelverBricks);
        }
    }
    s.set(2, 0, 2, block::LoreTablet);
    s.set(2, -1, 2, block::DelverBricks);
    s.point("portal", 0, 0, 0, 2, tag);
    s.point("inscripcion", 1, 0, 2, 3, tag == "superficie" ? std::string("2") : "subida:" + tag);
}

// Borde del agujero de una capa (sima_honda; local y 0 = sobre la plataforma, centro del agujero):
// seis pilares runicos (el primero con la tablilla de lo que hay abajo) y glifos arriba.
void buildShaftRim(const Sink& s, float r, float phase, int next_layer) {
    for (int i = 0; i < 6; ++i) {
        const float a = phase + static_cast<float>(i) * kPi / 3.0f;
        const int px = static_cast<int>(std::lround(std::cos(a) * (r + 3.0f))), pz = static_cast<int>(std::lround(std::sin(a) * (r + 3.0f)));
        const int h = 3 + static_cast<int>(s.rnd(i, 9) * 3.0f);
        for (int k = 0; k < h; ++k) s.set(px, k, pz, (i == 0 && k == 0) ? block::LoreTablet : block::RuneStone);
        s.set(px, h, pz, block::GlyphStone);
        if (i == 0) {
            const int tx = static_cast<int>(std::lround(std::cos(a) * (r + 4.2f))), tz = static_cast<int>(std::lround(std::sin(a) * (r + 4.2f)));
            s.point("inscripcion", tx, 0, tz, dirToward(-std::cos(a), -std::sin(a)), "capa:" + std::to_string(next_layer));
        }
    }
}

// Casa de piedra de la ciudad (de x0, z0 a x1, z1; puerta en (dx, dz) del muro): suelo de tablas,
// tejado de pizarra, ventanas y farol dentro.
void stoneHouse(const Sink& s, int x0, int z0, int x1, int z1, int h, int dx, int dz, BlockId wall, bool ruin) {
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const bool edge = x == x0 || x == x1 || z == z0 || z == z1;
            s.set(x, -1, z, edge ? block::DelverBricks : block::DarkPlanks);
            for (int y = 0; y < h; ++y) {
                if (!edge) {
                    s.set(x, y, z, block::Air);
                    continue;
                }
                if (ruin && y > 1 && s.rnd(x, y + 70, z) < 0.3f) {
                    s.set(x, y, z, block::Air);
                    continue;
                }
                const bool corner = (x == x0 || x == x1) && (z == z0 || z == z1);
                const bool window = !corner && y == 2 && ((x - x0) % 3 == 2 || (z - z0) % 3 == 2);
                s.set(x, y, z, corner ? block::DelverBricks : (window ? block::Glass : wall));
            }
            if (!ruin || s.rnd(x, 90, z) > 0.45f) s.set(x, h, z, block::RoofSlate);
        }
    if (!ruin) s.set((x0 + x1) / 2, h - 1, (z0 + z1) / 2, block::Lantern);
    s.set(dx, 0, dz, ruin ? block::Air : block::Door);
    s.set(dx, 1, dz, ruin ? block::Air : block::Door);
}

// Ciudad de los Delvers (capas 3+): muralla cuadrada de 81 m con cuatro puertas y torres, avenidas en cruz,
// plaza con fuente y piedra de vuelta, mercado (NE), forjas (NO), casas (SE) y el gran salon (SO; palacio en
// la capital). `ruin`: ciudad caida (sin gente, derrumbes, guarnicion y jefe en el salon).
void buildCity(const Sink& s, bool ruin, bool capital, int lore) {
    const auto wallSet = [&](int x, int y, int z, BlockId id) {
        if (ruin && y > 1 && s.rnd(x, y + 300, z) < 0.18f + 0.03f * static_cast<float>(y)) s.set(x, y, z, block::Air);
        else s.set(x, y, z, id);
    };
    // Suelo: avenidas, plaza y ronda empedradas; el resto, ladrillo y grava.
    for (int z = -40; z <= 40; ++z)
        for (int x = -40; x <= 40; ++x) {
            const int m = std::max(std::abs(x), std::abs(z));
            const bool street = std::abs(x) <= 2 || std::abs(z) <= 2 || m <= 10 || (m >= 33 && m <= 35);
            s.set(x, -1, z, street ? block::Paving : (s.rnd(x, z, 2) < 0.15f ? block::Gravel : block::DelverBricks));
            for (int y = 0; y <= 2; ++y) s.set(x, y, z, block::Air);
        }
    // Muralla (2 de grueso, almenas) con las cuatro puertas en medio de cada lado.
    for (int z = -40; z <= 40; ++z)
        for (int x = -40; x <= 40; ++x) {
            const int m = std::max(std::abs(x), std::abs(z));
            if (m < 39) continue;
            const bool gate = std::abs(x) <= 2 || std::abs(z) <= 2;
            for (int y = 0; y <= 8; ++y) {
                if (gate && y <= 4) {
                    s.set(x, y, z, block::Air);
                    continue;
                }
                if (y == 8) {
                    if (m == 40 && ((x + z) & 1) == 0) wallSet(x, y, z, block::DelverBricks);
                    continue;
                }
                wallSet(x, y, z, y == 5 && (x + z) % 4 == 0 ? block::GlyphStone : block::DelverBricks);
            }
        }
    const auto tower = [&](int cx, int cz, int half, int h) {
        for (int y = 0; y <= h; ++y)
            for (int dz = -half; dz <= half; ++dz)
                for (int dx = -half; dx <= half; ++dx) {
                    if (y == h && (std::abs(dx) != half || std::abs(dz) != half)) continue;  // almenas en las esquinas
                    wallSet(cx + dx, y, cz + dz, y == h - 2 && (dx == 0 || dz == 0) ? block::GlyphStone : block::DelverBricks);
                }
    };
    static constexpr int kGate[4][2] = {{0, -40}, {0, 40}, {40, 0}, {-40, 0}};
    for (const auto& g : kGate) {
        const int ux = g[1] != 0 ? 1 : 0, uz = g[0] != 0 ? 1 : 0;  // a lo largo del muro
        tower(g[0] + ux * 5, g[1] + uz * 5, 2, 11);
        tower(g[0] - ux * 5, g[1] - uz * 5, 2, 11);
        const int ix = g[0] == 0 ? 0 : (g[0] > 0 ? -1 : 1), iz = g[1] == 0 ? 0 : (g[1] > 0 ? -1 : 1);  // hacia dentro
        if (!ruin) {
            s.set(g[0] + ix * 2 + ux * 3, 0, g[1] + iz * 2 + uz * 3, block::StandingTorch);
            s.set(g[0] + ix * 2 - ux * 3, 0, g[1] + iz * 2 - uz * 3, block::StandingTorch);
            // Dos guardias en cada puerta, por dentro.
            const int gx = g[0] + ix * 4, gz = g[1] + iz * 4;
            const int dir = dirToward(static_cast<float>(-ix), static_cast<float>(-iz));
            s.point("npc", gx + ux * 3, 0, gz + uz * 3, dir, "guardia");
            s.point("npc", gx - ux * 3, 0, gz - uz * 3, dir, "guardia");
        } else {
            s.point("spawner", g[0] + ix * 5, 0, g[1] + iz * 5, 0, "");
        }
        s.point("entrada", g[0] - ix, 0, g[1] - iz, 0);
    }
    for (const int cx : {-40, 40})
        for (const int cz : {-40, 40}) tower(cx, cz, 3, 13);
    // Farolas por las avenidas (en la caida, braseros de almas sueltos).
    for (const int k : {12, 20, 28}) {
        if (!ruin) {
            lampPost(s, 3, k);
            lampPost(s, -3, -k);
            lampPost(s, k, -3);
            lampPost(s, -k, 3);
        } else if (s.rnd(k, 5) < 0.6f) {
            s.set(3, 0, k, block::SoulBrazier);
            s.set(-k, 0, 3, block::SoulBrazier);
        }
    }
    // Plaza: fuente con pilar de glifos, bancos, la piedra de vuelta y la tablilla.
    for (int z = -5; z <= 5; ++z)
        for (int x = -5; x <= 5; ++x) {
            const float d = std::hypot(static_cast<float>(x), static_cast<float>(z));
            if (d <= 3.0f) s.set(x, 0, z, ruin ? block::Gravel : block::PoolWater);
            else if (d <= 4.2f) s.set(x, 0, z, block::CarvedStone);
        }
    for (int y = 0; y <= 3; ++y) s.set(0, y, 0, block::MarblePillar);
    s.set(0, 4, 0, block::GlyphStone);
    for (const int k : {-7, 7}) {
        s.set(k, 0, -2, block::Bench);
        s.set(k, 0, 2, block::Bench);
    }
    s.point("plaza", 0, 0, -7, 0);
    for (int z = 6; z <= 8; ++z)
        for (int x = 6; x <= 8; ++x) s.set(x, -1, z, block::DelverBricks);
    for (const int x : {5, 9}) {
        for (int y = 0; y <= 2; ++y) s.set(x, y, 7, block::RuneStone);
        s.set(x, 3, 7, block::GlyphStone);
    }
    s.point("portal", 7, 0, 7, 0, "superficie");
    s.set(-7, 0, -7, block::LoreTablet);
    s.point("inscripcion", -7, 0, -6, 2, std::to_string(lore));
    // Mercado (NE): seis puestos con toldo y la taberna.
    static const char* const kShops[6] = {"profunda", "pociones", "armas", "armaduras", "general", "profunda"};
    static constexpr BlockId kCloth[4] = {block::ClothRed, block::ClothBlue, block::ClothYellow, block::ClothWhite};
    for (int i = 0; i < 6; ++i) {
        const int sx = 12 + (i % 3) * 8, sz = i < 3 ? -14 : -24;
        s.set(sx, 0, sz, ruin ? block::Crate : block::Counter);
        s.set(sx + 1, 0, sz, block::Counter);
        for (const int px : {sx - 1, sx + 2})
            for (const int pz : {sz - 1, sz + 1})
                for (int y = 0; y <= 2; ++y) s.set(px, y, pz, block::Fence);
        if (!ruin || i % 2 == 0) s.box(sx - 1, 3, sz - 1, sx + 2, 3, sz + 1, kCloth[i % 4]);
        if (!ruin) {
            s.set(sx + 1, 1, sz, block::Lantern);
            s.point("mercader", sx, 0, sz + 1, 0, kShops[i]);
        }
    }
    stoneHouse(s, 24, -37, 35, -30, 5, 29, -30, block::Timber, ruin);
    s.set(26, 0, -35, block::Barrel);
    s.set(27, 0, -35, block::Barrel);
    s.set(31, 0, -34, block::Table);
    s.set(33, 0, -34, block::Table);
    if (!ruin) s.point("npc", 29, 0, -33, 2, "tabernero");
    // Forjas (NO): tres talleres abiertos con fragua, yunque, lava en una pila y armeros.
    static constexpr int kForge[3][2] = {{-14, -15}, {-28, -15}, {-21, -28}};
    for (int i = 0; i < 3; ++i) {
        const int fx = kForge[i][0], fz = kForge[i][1];
        for (int z = fz - 3; z <= fz + 3; ++z)
            for (int x = fx - 4; x <= fx + 4; ++x) {
                s.set(x, -1, z, block::CarvedStone);
                const bool back = z == fz - 3, post = (x == fx - 4 || x == fx + 4) && (z == fz + 3 || z == fz - 3);
                for (int y = 0; y <= 3; ++y) {
                    if (back || post) wallSet(x, y, z, block::DelverBricks);
                    else s.set(x, y, z, block::Air);
                }
                if (!ruin || s.rnd(x, 50, z) > 0.5f) s.set(x, 4, z, block::RoofSlate);
            }
        s.set(fx - 2, 0, fz - 2, block::Forge);
        s.set(fx + 2, 0, fz - 2, block::WeaponRack);
        s.set(fx, 0, fz, block::Anvil);
        s.set(fx - 1, -1, fz - 2, block::Lava);
        s.set(fx + 3, 0, fz - 2, block::Barrel);
        if (!ruin) s.point("npc", fx + 1, 0, fz + 1, 0, i == 2 ? "armero" : "herrero");
        else s.point("spawner", fx, 0, fz + 1, 0, "");
    }
    // Casas (SE) con sus vecinos y un huerto de setas al fondo.
    static const char* const kFolk[6] = {"minero", "granjero", "nino", "anciano", "alquimista", "comerciante"};
    for (int i = 0; i < 6; ++i) {
        const int hx = 12 + (i % 3) * 10, hz = i < 3 ? 13 : 24;
        stoneHouse(s, hx - 3, hz - 3, hx + 3, hz + 3, 4, hx, hz - 3, i % 2 ? block::Plaster : block::StoneBricks, ruin);
        s.set(hx + 1, 0, hz + 2, ruin ? block::Bones : block::Bed);
        s.set(hx - 2, 0, hz + 1, block::Table);
        s.set(hx - 2, 0, hz + 2, i % 2 ? block::Barrel : block::Bookshelf);
        if (!ruin) {
            s.point("npc", hx, 0, hz, 0, kFolk[i]);
            s.point("cama", hx + 1, 0, hz + 2, 0);
        } else if (i % 2 == 0) {
            s.point("spawner", hx, 0, hz, 0, "");
        }
        s.point("entrada", hx, 0, hz - 4, 0);
    }
    for (int z = 31; z <= 35; ++z)
        for (int x = 8; x <= 32; ++x) {
            if (x % 6 == 2) continue;
            s.set(x, -1, z, block::DuskSoil);
            const float r = s.rnd(x, z, 6);
            s.set(x, 0, z, ruin ? (r < 0.2f ? block::Cobweb : block::Air)
                                : (r < 0.4f ? block::GlowMushroom : (r < 0.7f ? block::BrownMushroom : block::RedMushroom)));
        }
    // Gran salon (SO; palacio en la capital): columnas, alfombra, estrado con trono, estandartes y lamparas.
    const int h = capital ? 13 : 9;
    for (int z = 9; z <= 34; ++z)
        for (int x = -35; x <= -8; ++x) {
            const bool edge = x == -35 || x == -8 || z == 9 || z == 34;
            s.set(x, -1, z, block::DelverBricks);
            for (int y = 0; y < h; ++y) {
                if (!edge) {
                    s.set(x, y, z, block::Air);
                    continue;
                }
                if (x == -8 && z >= 20 && z <= 22 && y <= 4) {  // la puerta, hacia la avenida
                    s.set(x, y, z, block::Air);
                    continue;
                }
                wallSet(x, y, z, (y == 4 || y == h - 3) && (x + z) % 3 == 0 ? block::GlyphStone : block::DelverBricks);
            }
            if (!ruin || s.rnd(x, 91, z) > 0.4f) s.set(x, h, z, (x + z) % 7 == 0 && !edge ? block::GlyphStone : block::DelverBricks);
        }
    for (int x = -31; x <= -11; x += 4)
        for (const int z : {14, 29})
            for (int y = 0; y < h; ++y) wallSet(x, y, z, y == h - 1 ? block::GlyphStone : block::MarblePillar);
    for (int x = -30; x <= -9; ++x) s.set(x, -1, 21, block::RugRed);
    s.box(-34, 0, 18, -31, 0, 24, block::DelverBricks);
    s.set(-33, 1, 21, block::Throne);
    for (const int z : {17, 25}) {
        s.set(-34, 3, z, block::BannerBlue);
        s.set(-34, 4, z, block::BannerBlue);
    }
    for (int x = -30; x <= -12; x += 6) {
        if (!ruin) s.set(x, h - 1, 21, block::Chandelier);
        else s.set(x, 0, s.rnd(x, 3) < 0.5f ? 17 : 25, block::SoulBrazier);
    }
    s.set(-35, 2, 13, block::LoreTablet);
    if (capital) {
        // Palacio: torres en las esquinas y oro junto al trono.
        tower(-35, 9, 2, h + 6);
        tower(-35, 34, 2, h + 6);
        tower(-8, 9, 2, h + 6);
        tower(-8, 34, 2, h + 6);
        for (const int z : {19, 23}) s.set(-34, 1, z, block::GoldBlock);
        s.set(-34, 2, 21, block::GoldBlock);
    }
    if (!ruin) {
        s.point("npc", -31, 1, 21, 1, "anciano");
        s.point("npc", -20, 0, 12, 2, "sacerdote");
        s.point("npc", -12, 0, 18, 1, "capitan");
        s.point("npc", -6, 0, 18, 1, "guardia");
        s.point("npc", -6, 0, 24, 1, "guardia");
        if (capital) {
            s.point("npc", -28, 0, 17, 1, "guardia");
            s.point("npc", -28, 0, 25, 1, "guardia");
            s.point("npc", -16, 0, 30, 0, "mago");
        }
    } else {
        s.point("jefe", -29, 0, 21, 3, "");
        s.point("spawner", -20, 0, 17, 0, "");
        s.point("spawner", -20, 0, 25, 0, "");
        s.point("spawner", -14, 0, 21, 0, "");
        s.point("cofre", -33, 0, 18, 1, "3");
        s.point("cofre", -33, 0, 24, 1, "4");
        s.point("cofre", 29, 0, -33, 0, "2");
        for (int i = 0; i < 40; ++i) {
            const int x = -38 + static_cast<int>(s.rnd(i, 80) * 77.0f), z = -38 + static_cast<int>(s.rnd(i, 81) * 77.0f);
            s.soft(x, 0, z, s.rnd(i, 82) < 0.5f ? block::Bones : block::Cobweb);
        }
    }
    s.point("inscripcion", -34, 0, 13, 1, std::to_string(lore));
}

// Guarida del Corazon de Raiz (capa 8): arena redonda con raices vivas que se arquean hasta el Corazon
// (raiz y cristal), braseros, el jefe final, tesoro, la reliquia y la tablilla del final.
void buildLair(const Sink& s) {
    for (int z = -30; z <= 30; ++z)
        for (int x = -30; x <= 30; ++x) {
            const float d = std::hypot(static_cast<float>(x), static_cast<float>(z));
            if (d > 29.5f) continue;
            const float ring = std::fmod(d, 6.0f);
            s.set(x, -1, z, ring < 0.9f ? block::GlyphStone : (s.rnd(x, z, 3) < 0.3f ? block::CrackedBlackstone : block::Obsidian));
        }
    // Raices: de la pared al Corazon, en arco.
    for (int i = 0; i < 7; ++i) {
        const float a = static_cast<float>(i) * 2.0f * kPi / 7.0f + s.rnd(i, 1) * 0.3f;
        for (float t = 0.0f; t <= 1.0f; t += 0.02f) {
            const float rr = 28.0f * (1.0f - t) + 3.0f * t;
            const float px = std::cos(a) * rr, pz = std::sin(a) * rr;
            const float py = 2.0f + 20.0f * std::sin(t * kPi) * (1.0f - 0.5f * t);
            const float th = 1.7f - 0.6f * t;
            const int ir = static_cast<int>(std::ceil(th));
            for (int oy = -ir; oy <= ir; ++oy)
                for (int oz = -ir; oz <= ir; ++oz)
                    for (int ox = -ir; ox <= ir; ++ox)
                        if (static_cast<float>(ox * ox + oy * oy + oz * oz) <= th * th)
                            s.set(static_cast<int>(std::lround(px)) + ox, static_cast<int>(std::lround(py)) + oy, static_cast<int>(std::lround(pz)) + oz,
                                  block::HeartRoot);
        }
    }
    // El Corazon: bola de raiz con puntas de cristal rosa.
    for (int y = 0; y <= 9; ++y)
        for (int z = -4; z <= 4; ++z)
            for (int x = -4; x <= 4; ++x) {
                const float d = std::sqrt(static_cast<float>(x * x + z * z) + static_cast<float>((y - 5) * (y - 5)) * 0.8f);
                if (d <= 3.6f) s.set(x, y, z, d < 2.0f ? block::CrystalPink : block::HeartRoot);
            }
    for (int y = 10; y <= 15; ++y) s.set(0, y, 0, block::CrystalPink);
    for (int i = 0; i < 4; ++i) {
        const int dx = (i & 1) ? 1 : -1, dz = (i & 2) ? 1 : -1;
        for (int k = 0; k < 4; ++k) s.set(dx * (3 + k), 6 + k, dz * (3 + k), block::CrystalPink);
    }
    for (int i = 0; i < 10; ++i) {
        const float a = static_cast<float>(i) * kPi / 5.0f;
        const int x = static_cast<int>(std::lround(std::cos(a) * 24.0f)), z = static_cast<int>(std::lround(std::sin(a) * 24.0f));
        for (int y = 0; y <= 2; ++y) s.set(x, y, z, block::BlackstoneBricks);
        s.set(x, 3, z, block::SoulBrazier);
    }
    // Altar del final, tesoro y la piedra de vuelta detras.
    s.box(-3, 0, -16, 3, 0, -13, block::BlackstoneBricks);
    s.set(0, 1, -15, block::Altar);
    s.set(0, 1, -16, block::LoreTablet);
    s.point("inscripcion", 0, 1, -14, 2, "27");
    s.point("reliquia", 0, 1, -13, 2, "raiz");
    for (const int x : {-6, 6}) s.set(x, 0, -14, block::GoldBlock);
    for (int z = -24; z <= -20; ++z)
        for (int x = -2; x <= 2; ++x) s.set(x, -1, z, block::DelverBricks);
    for (const int x : {-2, 2}) {
        for (int y = 0; y <= 2; ++y) s.set(x, y, -22, block::RuneStone);
        s.set(x, 3, -22, block::GlyphStone);
    }
    s.point("portal", 0, 0, -22, 2, "superficie");
    s.point("jefe", 0, 0, 11, 0, "dragon");
    s.point("spawner", 14, 0, 6, 3, "espectro");
    s.point("spawner", -14, 0, 6, 1, "espectro");
    s.point("spawner", 10, 0, -8, 3, "caballero_espectral");
    s.point("spawner", -10, 0, -8, 1, "caballero_espectral");
    s.point("spawner", 0, 0, 20, 0, "golem");
    s.point("cofre", -8, 0, -17, 2, "4");
    s.point("cofre", 8, 0, -17, 2, "4");
    s.point("cofre", 0, 0, -18, 2, "4");
    s.point("entrada", 0, 0, 29, 0);
}

std::string pick(const char* const* list, int n, std::uint32_t h) { return list[h % static_cast<std::uint32_t>(n)]; }

}  // namespace

// --- Cache -----------------------------------------------------------------------------

struct Generator::DeepSite {
    int kind = kNone;
    int x = 0, y = 0, z = 0;  // ancla (centro, primer bloque de aire sobre el suelo)
    int rot = 0;
    std::uint32_t seed = 0;
    int hx = 8, hz = 8, h = 10;  // zona que se vacia (semiejes y alto); h 0: no se vacia
    BlockId floor = block::Air;  // suelo de la zona vaciada (Air: el que haya)
    int lore = 0;
    int pattern = 0;
    int colors[4] = {0, 1, 2, 3};
    std::string tag = "superficie";  // Piedra: a donde lleva
    int depth = 28;                  // Piedra: lo que baja el pilar
    bool ruin = false, capital = false;  // ciudad caida / capital (palacio)
    bool square = false;             // zona vaciada cuadrada (ciudades)
    float r = 7.0f, phase = 0.0f;    // agujero del suelo (sima_honda)
    int next = 2;                    // agujero: la capa de abajo
    StructureInfo info;  // con sus puntos
};

struct Generator::DeepCache {
    std::mutex mutex;
    std::unordered_map<std::int64_t, Sinkhole> sinks;
    std::unordered_map<std::int64_t, std::vector<std::shared_ptr<const DeepSite>>> regions;
};

std::shared_ptr<Generator::DeepCache> Generator::makeDeepCache() { return std::make_shared<DeepCache>(); }

void Generator::setDeepLayer(int layer) {
    deep_layer_ = std::clamp(layer, 1, kDeepLayers);
    if (deep_cache_) {
        std::lock_guard<std::mutex> lock(deep_cache_->mutex);
        deep_cache_->regions.clear();
    }
}

// --- Pozos entre capas ------------------------------------------------------------------

Generator::Sinkhole Generator::shaftCandidate(int boundary, int gx, int gz) const {
    Sinkhole s;
    if (boundary < 1 || boundary >= kDeepLayers) return s;
    const auto seed = static_cast<std::uint32_t>(seed_) + 6100u + static_cast<std::uint32_t>(boundary) * 977u;
    if (rand01(gx, 0, gz, seed) > 0.85f) return s;
    // Las fronteras impares, con la rejilla corrida media celda (los pozos de arriba y de abajo de una
    // capa no caen en el mismo sitio).
    const int shift = (boundary & 1) ? kShaftCell / 2 : 0;
    s.x = gx * kShaftCell + shift + 80 + static_cast<int>(rand01(gx, 1, gz, seed) * 224.0f);
    s.z = gz * kShaftCell + shift + 80 + static_cast<int>(rand01(gx, 2, gz, seed) * 224.0f);
    s.r = 6.0f + rand01(gx, 3, gz, seed) * 2.0f;
    s.phase = rand01(gx, 4, gz, seed) * 2.0f * kPi;
    s.id = hash3(gx, 5 + boundary, gz, seed);
    s.valid = true;
    return s;
}

Generator::Sinkhole Generator::layerShaft(int boundary, int gx, int gz) const {
    Sinkhole s = shaftCandidate(boundary, gx, gz);
    if (!s.valid) return s;
    // Lejos de lo que baja del techo en esa capa: las simas de la superficie (capa 1; basta el
    // candidato, sin mirar si es valida) o los pozos de la frontera de arriba.
    if (boundary == 1) {
        const auto seed = static_cast<std::uint32_t>(seed_) + 4100u;  // como sinkhole()
        for (int cz = floorDiv(s.z - 120, kSinkCell); cz <= floorDiv(s.z + 120, kSinkCell); ++cz)
            for (int cx = floorDiv(s.x - 120, kSinkCell); cx <= floorDiv(s.x + 120, kSinkCell); ++cx) {
                if (rand01(cx, 0, cz, seed) >= 0.62f) continue;
                const int x = cx * kSinkCell + 120 + static_cast<int>(rand01(cx, 1, cz, seed) * 400.0f);
                const int z = cz * kSinkCell + 120 + static_cast<int>(rand01(cx, 2, cz, seed) * 400.0f);
                if (std::hypot(static_cast<float>(x - s.x), static_cast<float>(z - s.z)) < 110.0f) return {};
            }
    } else {
        const int shift = ((boundary - 1) & 1) ? kShaftCell / 2 : 0;
        for (int cz = floorDiv(s.z - 100 - shift, kShaftCell); cz <= floorDiv(s.z + 100 - shift, kShaftCell); ++cz)
            for (int cx = floorDiv(s.x - 100 - shift, kShaftCell); cx <= floorDiv(s.x + 100 - shift, kShaftCell); ++cx) {
                const Sinkhole o = shaftCandidate(boundary - 1, cx, cz);
                if (o.valid && std::hypot(static_cast<float>(o.x - s.x), static_cast<float>(o.z - s.z)) < 100.0f) return {};
            }
    }
    // En su capa: el suelo de la plataforma del agujero (por encima del mar, con sitio debajo para caer).
    if (boundary == deep_layer_) {
        const DeepTheme& th = themeOf(deep_layer_);
        const DeepColumn c = deepColumn(s.x, s.z, nullptr, nullptr);
        s.top = std::clamp(std::max({c.lf, th.sea + 4, 30}), 30, 96);
    }
    return s;
}

Generator::Sinkhole Generator::layerShaftAt(int boundary, int x, int z) const {
    const int shift = (boundary & 1) ? kShaftCell / 2 : 0;
    return layerShaft(boundary, floorDiv(x - shift, kShaftCell), floorDiv(z - shift, kShaftCell));
}

namespace {
// Escalera de los Delvers: espiral de escalones por la pared del pozo del techo (capas 2..8), del lago
// (base) hasta un rellano a kStairTop; una vuelta sube kStairTurn bloques.
constexpr int kStairTurn = 24;
constexpr int kStairTop = kWorldHeight - 12;
struct StairEnd {
    float x = 0.0f, z = 0.0f;
    int y = 0;
};
float stairTurns(int base) { return static_cast<float>(kStairTop - base) / static_cast<float>(kStairTurn); }
StairEnd stairEnd(const Generator::Sinkhole& f, int base) {
    const float total = stairTurns(base);
    const float t = total - std::floor(total);
    const float a = f.phase + t * 2.0f * kPi;
    StairEnd e;
    e.x = static_cast<float>(f.x) + 0.5f + std::cos(a) * (f.r + 2.1f);
    e.z = static_cast<float>(f.z) + 0.5f + std::sin(a) * (f.r + 2.1f);
    e.y = std::max(base, kStairTop);  // entero: igual aqui y en el chunk (nada de redondeos)
    return e;
}
}  // namespace
// --- Simas --------------------------------------------------------------------------------

Generator::Sinkhole Generator::sinkhole(int gx, int gz) const {
    if (!deep_cache_) return {};
    const std::int64_t key = cellKey(gx, gz);
    {
        std::lock_guard<std::mutex> lock(deep_cache_->mutex);
        const auto it = deep_cache_->sinks.find(key);
        if (it != deep_cache_->sinks.end()) return it->second;
    }
    // Se calcula sin el candado (puede planificar estructuras de la superficie); si dos
    // hilos lo hacen a la vez sale lo mismo.
    Sinkhole s;
    const auto seed = static_cast<std::uint32_t>(seed_) + 4100u;
    if (rand01(gx, 0, gz, seed) < 0.62f) {
        s.x = gx * kSinkCell + 120 + static_cast<int>(rand01(gx, 1, gz, seed) * 400.0f);
        s.z = gz * kSinkCell + 120 + static_cast<int>(rand01(gx, 2, gz, seed) * 400.0f);
        s.r = 6.5f + rand01(gx, 3, gz, seed) * 2.5f;
        s.phase = rand01(gx, 4, gz, seed) * 2.0f * kPi;
        s.id = hash3(gx, 5, gz, seed);
        const double d2 = static_cast<double>(s.x) * s.x + static_cast<double>(s.z) * s.z;
        const Generator& g = surfaceGenerator();
        if (d2 > 1100.0 * 1100.0 && g.profile() == VoxelGenerator::Adventure) {
            const AdventureColumn c = g.adventureColumn(s.x, s.z);
            bool ok = c.water <= c.height && !c.lake && c.river < 0.15f && c.lava <= c.height && c.height >= g.seaLevel() + 3 &&
                      c.height <= 170;
            // Lejos de los rios y del agua alrededor (el brocal no se inunda).
            for (int i = 0; ok && i < 8; ++i) {
                const float a = static_cast<float>(i) * kPi / 4.0f;
                const AdventureColumn e = g.adventureColumn(s.x + static_cast<int>(std::cos(a) * (s.r + 6.0f)),
                                                            s.z + static_cast<int>(std::sin(a) * (s.r + 6.0f)));
                ok = e.water <= e.height && e.river < 0.3f && e.height >= g.seaLevel() + 2;
            }
            if (ok && g.structures() != nullptr)
                ok = g.structures()->nearby(static_cast<float>(s.x), static_cast<float>(c.height), static_cast<float>(s.z), s.r + 30.0f).empty();
            if (ok) {
                s.top = g.terrainHeight(s.x, s.z);
                s.valid = true;
            }
        }
    }
    std::lock_guard<std::mutex> lock(deep_cache_->mutex);
    if (deep_cache_->sinks.size() > 4096) deep_cache_->sinks.clear();
    deep_cache_->sinks.emplace(key, s);
    return s;
}

namespace {
// Distancia "irregular" al centro de la sima (el borde no es un circulo perfecto).
float sinkDistance(const Generator::Sinkhole& s, float x, float z) {
    const float dx = x - static_cast<float>(s.x), dz = z - static_cast<float>(s.z);
    const float a = std::atan2(dz, dx);
    return std::hypot(dx, dz) * (1.0f + 0.08f * std::sin(a * 3.0f + s.phase) + 0.04f * std::sin(a * 7.0f + s.phase * 2.0f));
}
const char* const kMawNames[] = {"Weeping", "Endless", "Hollow", "Sunless", "Black", "Silent", "Whispering", "Starless"};

StructureInfo sinkholeInfo(const Generator::Sinkhole& s, int world_seed) {
    StructureInfo info;
    info.id = (std::uint64_t{1} << 51) | ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.x)) & 0xFFFFFFu) << 24) |
              (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.z)) & 0xFFFFFFu);
    info.type = "sima_abisal";
    info.name = std::string("The ") + kMawNames[s.id % 8u] + " Maw";
    info.center = core::Vec3{static_cast<float>(s.x) + 0.5f, static_cast<float>(s.top), static_cast<float>(s.z) + 0.5f};
    const float e = s.r + 5.0f;
    info.min = core::Vec3{static_cast<float>(s.x) - e, 3.0f, static_cast<float>(s.z) - e};
    info.max = core::Vec3{static_cast<float>(s.x) + e + 1.0f, static_cast<float>(s.top + 8), static_cast<float>(s.z) + e + 1.0f};
    info.level = 6;
    info.seed = s.id ^ static_cast<std::uint32_t>(world_seed);
    info.dimension = "superficie";
    StructurePoint portal;
    portal.kind = "portal";
    portal.tag = "profundo";
    portal.position = core::Vec3{static_cast<float>(s.x) + 0.5f, 11.0f, static_cast<float>(s.z) + 0.5f};
    info.points.push_back(portal);
    // La tablilla del primer pilar del brocal.
    const int px = s.x + static_cast<int>(std::lround(std::cos(s.phase) * (s.r + 3.0f)));
    const int pz = s.z + static_cast<int>(std::lround(std::sin(s.phase) * (s.r + 3.0f)));
    StructurePoint tablet;
    tablet.kind = "inscripcion";
    tablet.tag = "1";
    tablet.position = core::Vec3{static_cast<float>(px) + 0.5f, static_cast<float>(s.top + 1), static_cast<float>(pz) + 0.5f};
    info.points.push_back(tablet);
    return info;
}
}  // namespace

void Generator::carveSinkholes(ChunkData& chunk, const std::array<int, kChunkSize * kChunkSize>& heights) const {
    if (!deep_cache_ || profile_ != VoxelGenerator::Adventure || !immense_caves_) return;
    const int x0 = chunk.cx * kChunkSize, z0 = chunk.cz * kChunkSize;
    const Sinkhole s = sinkhole(floorDiv(x0, kSinkCell), floorDiv(z0, kSinkCell));
    if (!s.valid) return;
    const float reach = s.r + 6.0f;
    const float nx = std::clamp(static_cast<float>(s.x), static_cast<float>(x0), static_cast<float>(x0 + kChunkSize - 1));
    const float nz = std::clamp(static_cast<float>(s.z), static_cast<float>(z0), static_cast<float>(z0 + kChunkSize - 1));
    if (std::hypot(nx - static_cast<float>(s.x), nz - static_cast<float>(s.z)) > reach) return;
    const auto at = [&](int x, int y, int z) -> BlockId& { return chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))]; };
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            const int wx = x0 + x, wz = z0 + z;
            const float d = sinkDistance(s, static_cast<float>(wx) + 0.5f, static_cast<float>(wz) + 0.5f);
            if (d >= s.r + 4.5f) continue;
            const int ground = std::clamp(heights[static_cast<std::size_t>(x + z * kChunkSize)], 12, kWorldHeight - 30);
            if (d < s.r) {
                // El pozo: estanque de glifos en el fondo y aire hasta el cielo.
                for (int y = 0; y < kWorldHeight; ++y) {
                    BlockId id = block::Air;
                    if (y <= 2) id = block::Bedrock;
                    else if (y == 3) id = d > s.r - 1.6f ? block::GlyphStone : block::Obsidian;
                    else if (y <= 10) id = block::Water;
                    else if (y > ground + 24) break;
                    at(x, y, z) = id;
                }
            } else if (d < s.r + 2.2f) {
                // Pared: se tapan los liquidos de las cuevas (sus huecos de aire se ven desde el pozo).
                for (int y = 3; y <= ground; ++y) {
                    BlockId& c = at(x, y, z);
                    if (block::isWet(c) || c == block::Lava || (y <= 11 && !solidId(c))) c = block::Deepstone;
                    if (y % 14 == 6 && d < s.r + 1.0f && solidId(c) && y < ground - 3) c = block::GlyphStone;
                }
                at(x, ground, z) = block::CarvedStone;
                for (int y = ground + 1; y <= std::min(kWorldHeight - 1, ground + 4); ++y) {
                    if (!solidId(at(x, y, z)) || blockDef(at(x, y, z)).cutout) at(x, y, z) = block::Air;
                }
            } else if (solidId(at(x, ground, z))) {
                at(x, ground, z) = rand01(wx, 1, wz, static_cast<std::uint32_t>(seed_) + 4111u) < 0.7f ? block::CarvedStone : block::Cobblestone;
                if (ground + 1 < kWorldHeight && blockDef(at(x, ground + 1, z)).replaceable) at(x, ground + 1, z) = block::Air;
            }
        }
    }
    // Seis pilares runicos en el brocal (el primero con la tablilla) con glifos arriba.
    for (int i = 0; i < 6; ++i) {
        const float a = s.phase + static_cast<float>(i) * kPi / 3.0f;
        const int px = s.x + static_cast<int>(std::lround(std::cos(a) * (s.r + 3.0f)));
        const int pz = s.z + static_cast<int>(std::lround(std::sin(a) * (s.r + 3.0f)));
        const int lx = px - x0, lz = pz - z0;
        if (lx < 0 || lz < 0 || lx >= kChunkSize || lz >= kChunkSize) continue;
        const int ground = heights[static_cast<std::size_t>(lx + lz * kChunkSize)];
        const int h = 3 + static_cast<int>(hash3(i, 9, static_cast<int>(s.id), 77u) % 3u);
        for (int k = 1; k <= h && ground + k < kWorldHeight - 1; ++k) at(lx, ground + k, lz) = (i == 0 && k == 1) ? block::LoreTablet : block::RuneStone;
        if (ground + h + 1 < kWorldHeight) at(lx, ground + h + 1, lz) = block::GlyphStone;
    }
}

// --- Reino Profundo: columnas ------------------------------------------------------------

Generator::DeepColumn Generator::deepColumn(int x, int z, const Sinkhole* fall, const Sinkhole* hole) const {
    const DeepTheme& th = themeOf(deep_layer_);
    // Cada capa, otro trozo del ruido (la 1, el de siempre).
    const float fx = static_cast<float>(x) + layerOffX(deep_layer_), fz = static_cast<float>(z) + layerOffZ(deep_layer_);
    DeepColumn c;
    // Capa de abajo.
    const float ember = fbm2(fx / 520.0f, fz / 520.0f, 3, 1103);
    const float crystal = fbm2(fx / 520.0f, fz / 520.0f, 3, 1104);
    const float fungal = fbm2(fx / 300.0f, fz / 300.0f, 2, 1106);
    c.lower = ember > th.ember_t ? 3 : (crystal > th.crystal_t ? 2 : (fungal > th.fungal_t ? 1 : 0));
    float lf = 40.0f + fbm2(fx / 300.0f, fz / 300.0f, 4, 1101) * 34.0f + fbm2(fx / 70.0f, fz / 70.0f, 2, 1102) * 6.0f;
    const float ember_lo = th.ember_t - 0.06f, crystal_lo = th.crystal_t - 0.06f;
    if (ember > ember_lo)
        lf = lerp(lf, std::max(lf, 45.0f + fbm2(fx / 90.0f, fz / 90.0f, 2, 1109) * 4.0f), smoothstep(ember_lo, th.ember_t + 0.06f, ember));
    if (crystal > crystal_lo && c.lower == 2) {
        const float ridge = 1.0f - std::abs(fbm2(fx / 150.0f, fz / 150.0f, 3, 1105));
        lf += std::pow(ridge, 6.0f) * 85.0f * smoothstep(crystal_lo, th.crystal_t + 0.1f, crystal);
    }
    lf += th.floor_add;
    float lc = 120.0f + fbm2(fx / 160.0f, fz / 160.0f, 3, 1110) * 8.0f;
    const float st = std::max(0.0f, fbm2(fx / 9.0f, fz / 9.0f, 2, 1111));
    lc -= st * st * 45.0f * th.stal;
    // Corteza y capa de arriba.
    const float uf = 150.0f + fbm2(fx / 220.0f, fz / 220.0f, 3, 1112) * 9.0f + fbm2(fx / 50.0f, fz / 50.0f, 2, 1113) * 3.0f;
    const float ua = fbm2(fx / 480.0f, fz / 480.0f, 3, 1120), ub = fbm2(fx / 480.0f, fz / 480.0f, 3, 1121);
    c.upper = ua > th.up_crystal_t ? 2 : (ub > th.up_fungal_t ? 1 : 0);
    float uc = 236.0f + fbm2(fx / 180.0f, fz / 180.0f, 3, 1114) * 8.0f;
    const float st2 = std::max(0.0f, fbm2(fx / 8.0f, fz / 8.0f, 2, 1115));
    uc -= st2 * st2 * (c.upper == 0 ? 110.0f : 40.0f);
    const float ch = std::abs(fbm2(fx / 380.0f, fz / 380.0f, 3, 1116) + fbm2(fx / 90.0f, fz / 90.0f, 2, 1117) * 0.08f);
    // Simas entre las capas: grietas estrechas (y solo en la mitad del mapa, a trozos).
    c.chasm = ch < th.chasm_t && fbm2(fx / 600.0f, fz / 600.0f, 2, 1118) > -0.02f;
    // La caida bajo una sima (o el pozo de la capa de arriba): cuenca honda (lago) y el pozo abierto
    // del techo al agua. Sin mar de agua alrededor, un borde que encierra el lago.
    if (fall != nullptr && fall->valid) {
        const float d = sinkDistance(*fall, static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f);
        c.well_d = d;
        if (d < 26.0f) lf = std::min(lf, lerp(14.0f, lf, smoothstep(15.0f, 26.0f, d)));
        if ((th.sea < kDeepSea || th.liquid != block::Water) && d >= 24.0f && d < 36.0f)
            lf = std::max(lf, lerp(lf, static_cast<float>(kDeepSea + 2), smoothstep(36.0f, 30.0f, d)));
        if (d < fall->r + 1.0f) c.well = true;
    }
    // El agujero del suelo hacia la capa de abajo: plataforma llana alrededor (sobre el mar) con sitio encima.
    float hole_k = 0.0f;
    if (hole != nullptr && hole->valid) {
        const float d = sinkDistance(*hole, static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f);
        c.hole_d = d;
        hole_k = smoothstep(hole->r + 24.0f, hole->r + 11.0f, d);
        if (hole_k > 0.0f) {
            lf = lerp(lf, static_cast<float>(hole->top), hole_k);
            lc = std::max(lc, lerp(lc, static_cast<float>(hole->top + 22), hole_k));
        }
        if (d < hole->r) c.hole = true;
    }
    c.lf = std::clamp(static_cast<int>(std::lround(lf)), 6, 116);
    c.lc = std::max(c.lf + 1, static_cast<int>(std::lround(lc)));
    c.uf = std::max(c.lc + 6, static_cast<int>(std::lround(uf)));
    c.uc = std::clamp(static_cast<int>(std::lround(uc)), c.uf + 1, kWorldHeight - 6);
    if (c.lower == 3 && c.lf > th.sea + 1 && hole_k <= 0.0f) {
        const float rv = std::abs(fbm2(fx / 260.0f, fz / 260.0f, 3, 1107) + fbm2(fx / 60.0f, fz / 60.0f, 2, 1108) * 0.07f);
        c.river = 1.0f - smoothstep(0.015f, 0.035f, rv);
    }
    return c;
}
// --- Reino Profundo: estructuras ------------------------------------------------------

namespace {
const char* const kVillagePre[] = {"Lantern", "Glim", "Dusk", "Ember", "Hollow", "Moss", "Candle", "Gloam", "Wick", "Lumen"};
const char* const kVillageSuf[] = {"hollow", "holt", "mere", "burrow", "hearth", "glen", "cairn", "nook"};
const char* const kHallNames[] = {"Karr Umbral", "the Hollow Kings", "Varrok Deep", "Duur Thane", "the Ninth Delve", "Ostrum", "Brannhal"};
const char* const kTempleNames[] = {"the Four Flames", "the Sleeping Heart", "the Silent Choir", "the Last Lantern", "the Sealed Root"};
const char* const kSanctumNames[] = {"Sanctum of the Hollow Choir", "Shrine of the Hunger", "The Whispering Altar", "Sanctum of Ash Eyes"};
const char* const kColonyNames[] = {"Brightdelve", "Copperfall", "Lowmarch", "Pickwick", "Greystake", "Ironmoss", "Deepwatch"};
const char* const kObeliskNames[] = {"the First Delve", "Remembrance", "the Hollow Kings", "Warning", "the Root", "the Long Dark"};
}  // namespace

namespace {
const char* const kCityPre[] = {"Karak", "Dun", "Vael", "Thal", "Brann", "Kel", "Durn", "Orm", "Gal", "Hroth"};
const char* const kCitySuf[] = {"hold", "gate", "forge", "deep", "halls", "mor", "spire", "helm"};
const char* const kDropNames[] = {"Echoing", "Gloaming", "Rootward", "Ember", "Drowning", "Lantern", "Last", "Deepfall"};
// Ciudades por bloque de 3 x 3 regiones (capas 3+) y cuantas de ellas caidas (hostiles).
constexpr float kCityChance[Generator::kDeepLayers + 1] = {0.0f, 0.0f, 0.0f, 0.8f, 0.85f, 1.0f, 0.85f, 0.75f, 0.7f};
constexpr float kRuinChance[Generator::kDeepLayers + 1] = {0.0f, 0.0f, 0.0f, 0.15f, 0.2f, 0.2f, 0.45f, 0.65f, 0.6f};
}  // namespace

std::shared_ptr<const Generator::DeepSite> Generator::planDeepSite(int rx, int rz) const {
    const int L = deep_layer_;
    const DeepTheme& th = themeOf(L);
    const auto seed = static_cast<std::uint32_t>(seed_) + 5200u + (L > 1 ? static_cast<std::uint32_t>(L - 1) * 104729u : 0u);
    const float roll = rand01(rx, 0, rz, seed);
    int kind = kNone;
    // Capa 8: la guarida del Corazon, una por bloque de 4 x 4 regiones. Capas 3+: una ciudad por bloque de 3 x 3.
    if (L == kDeepLayers) {
        const int bx = floorDiv(rx, 4), bz = floorDiv(rz, 4);
        const std::uint32_t hb = hash3(bx, 91, bz, seed);
        if (rx == bx * 4 + static_cast<int>(hb % 4u) && rz == bz * 4 + static_cast<int>((hb >> 4) % 4u)) kind = kLair;
    }
    if (kind == kNone && L >= 3) {
        const int bx = floorDiv(rx, 3), bz = floorDiv(rz, 3);
        const std::uint32_t hb = hash3(bx, 77, bz, seed);
        if (rx == bx * 3 + static_cast<int>(hb % 3u) && rz == bz * 3 + static_cast<int>((hb >> 4) % 3u) && rand01(bx, 78, bz, seed) < kCityChance[L])
            kind = kCity;
    }
    if (kind == kNone) {
        if (roll < 0.14f) kind = kVillage;
        else if (roll < 0.24f) kind = kHermit;
        else if (roll < 0.37f) kind = kHall;
        else if (roll < 0.47f) kind = kTemple;
        else if (roll < 0.56f) kind = kSanctum;
        else if (roll < 0.65f) kind = kColony;
        else if (roll < 0.79f) kind = kObelisk;
    }
    if (kind == kNone) return nullptr;
    auto site = std::make_shared<DeepSite>();
    site->kind = kind;
    site->seed = hash3(static_cast<int>(hash3(rx, 11, rz, seed)), rz, rx, seed ^ 0x9E3779B9u);
    site->rot = static_cast<int>(rand01(rx, 3, rz, seed) * 4.0f) & 3;
    const bool big = kind == kCity || kind == kLair;
    site->ruin = kind == kCity && rand01(rx, 6, rz, seed) < kRuinChance[L];
    site->capital = kind == kCity && L == 5 && !site->ruin;
    switch (kind) {
        case kVillage: site->hx = 24; site->hz = 24; site->h = 15; site->floor = block::GlowMoss; break;
        case kHermit: site->hx = 8; site->hz = 8; site->h = 8; site->floor = block::DuskSoil; break;
        case kHall: site->hx = 22; site->hz = 14; site->h = 13; site->floor = block::DelverBricks; break;
        case kTemple: site->hx = 12; site->hz = 17; site->h = 13; site->floor = block::DelverBricks; break;
        case kSanctum: site->hx = 15; site->hz = 15; site->h = 11; site->floor = block::BlackstoneBricks; break;
        case kColony: site->hx = 18; site->hz = 18; site->h = 9; site->floor = block::Gravel; break;
        case kCity:
            site->hx = 43; site->hz = 43; site->h = site->capital ? 26 : 22; site->floor = block::DelverBricks; site->square = true;
            break;
        case kLair: site->hx = 31; site->hz = 31; site->h = 30; site->floor = block::Obsidian; break;
        default: site->hx = 5; site->hz = 5; site->h = 12; site->floor = block::Air; break;
    }
    // Lejos de las caidas (el lago y su calzada) y de los agujeros del suelo de esta capa.
    const float keep = big ? 105.0f : 90.0f;
    const auto clear = [&](int cx, int cz) {
        if (L == 1) {
            for (int gz = floorDiv(cz - 90, kSinkCell); gz <= floorDiv(cz + 90, kSinkCell); ++gz)
                for (int gx = floorDiv(cx - 90, kSinkCell); gx <= floorDiv(cx + 90, kSinkCell); ++gx) {
                    const Sinkhole s = sinkhole(gx, gz);
                    if (s.valid && std::hypot(static_cast<float>(s.x - cx), static_cast<float>(s.z - cz)) < 90.0f) return false;
                }
        }
        const int k = static_cast<int>(keep);
        for (int b = L - 1; b <= L; ++b) {
            if (b < 1 || b >= kDeepLayers) continue;
            const int shift = (b & 1) ? kShaftCell / 2 : 0;
            for (int gz = floorDiv(cz - k - shift, kShaftCell); gz <= floorDiv(cz + k - shift, kShaftCell); ++gz)
                for (int gx = floorDiv(cx - k - shift, kShaftCell); gx <= floorDiv(cx + k - shift, kShaftCell); ++gx) {
                    const Sinkhole s = shaftCandidate(b, gx, gz);
                    if (s.valid && std::hypot(static_cast<float>(s.x - cx), static_cast<float>(s.z - cz)) < keep) return false;
                }
        }
        return true;
    };
    // El sitio (las ciudades y la guarida prueban hasta cuatro): la capa (abajo, sobre el mar, o arriba) con sitio.
    int cx = 0, cz = 0, floor_y = -1;
    for (int attempt = 0; attempt < (big ? 4 : 1) && floor_y < 0; ++attempt) {
        cx = rx * kDeepRegion + kDeepRegion / 2 + static_cast<int>((rand01(rx, 1 + attempt * 7, rz, seed) - 0.5f) * 120.0f);
        cz = rz * kDeepRegion + kDeepRegion / 2 + static_cast<int>((rand01(rx, 2 + attempt * 7, rz, seed) - 0.5f) * 120.0f);
        if (!clear(cx, cz)) continue;
        const DeepColumn c = deepColumn(cx, cz, nullptr);
        const bool low_ok = c.lf >= th.sea + 1 && c.lc - c.lf >= site->h + 3 && c.river < 0.1f;
        const bool up_ok = !c.chasm && c.uc - c.uf >= site->h + 3;
        const bool prefer_low = rand01(rx, 4, rz, seed) < (th.sea > 50 ? 0.15f : 0.55f);
        if (low_ok && (prefer_low || !up_ok)) floor_y = c.lf;
        else if (up_ok) floor_y = c.uf;
    }
    if (floor_y < 0) return nullptr;
    site->x = cx;
    site->z = cz;
    site->y = floor_y + 1;
    const std::uint32_t h = site->seed;
    const float dist = std::hypot(static_cast<float>(cx), static_cast<float>(cz));
    StructureInfo& info = site->info;
    // Id unico por region y capa (bit 50: aparte de las de la superficie; capa en los bits 45..47;
    // < 2^52 para el juego).
    info.id = (std::uint64_t{1} << 50) | ((static_cast<std::uint64_t>(L - 1) & 7u) << 45) |
              ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(rx)) & 0xFFFFFu) << 25) |
              ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(rz)) & 0xFFFFFu) << 5) | static_cast<std::uint64_t>(kind);
    // Nivel (Mazmorras: monstruos de nivel 2 x nivel - 1): cada capa, 3 mas (unos 6 niveles de monstruo).
    info.level = 6 + 3 * (L - 1) + std::min(4, static_cast<int>(dist / 2000.0f));
    info.seed = h;
    info.dimension = layerDim(L);
    const int base = layerLore(L);  // tablillas de la capa (capas 2..8)
    switch (kind) {
        case kVillage:
            info.type = "aldea_oculta";
            info.name = pick(kVillagePre, 10, h) + pick(kVillageSuf, 8, h >> 8);
            break;
        case kHermit:
            info.type = "ermita";
            info.name = (h >> 4) % 2u ? "Hermit's Hollow" : "The Recluse's Cell";
            site->lore = L == 1 ? 7 + static_cast<int>((h >> 9) % 2u) : base + 1;
            break;
        case kHall:
            info.type = "salon_enano";
            info.name = "Hall of " + pick(kHallNames, 7, h);
            site->lore = L == 1 ? 9 : base;
            break;
        case kTemple: {
            info.type = "templo_runico";
            info.name = "Temple of " + pick(kTempleNames, 5, h);
            site->pattern = 1 + static_cast<int>((h >> 5) % 14u);
            // Colores de las cuatro lamparas: una permutacion.
            int order[4] = {0, 1, 2, 3};
            for (int i = 3; i > 0; --i) std::swap(order[i], order[(h >> (10 + i * 3)) % static_cast<std::uint32_t>(i + 1)]);
            for (int i = 0; i < 4; ++i) site->colors[i] = order[i];
            break;
        }
        case kSanctum:
            info.type = "santuario_culto";
            info.name = pick(kSanctumNames, 4, h);
            site->lore = L == 1 ? 11 : base;
            break;
        case kColony:
            info.type = "colonia_perdida";
            info.name = pick(kColonyNames, 7, h) + " Colony";
            site->lore = L == 1 ? 12 : base + 1;
            break;
        case kCity: {
            const std::string name = pick(kCityPre, 10, h) + pick(kCitySuf, 8, h >> 8);
            if (site->ruin) {
                info.type = "ciudad_caida";
                info.name = "Ruins of " + name;
                site->lore = base;
            } else {
                info.type = "ciudad_profunda";
                info.name = site->capital ? std::string("Orrun's Hold") : name;
                site->lore = base + 1;
            }
            break;
        }
        case kLair:
            info.type = "corazon_raiz";
            info.name = "The Heart of the Root";
            info.level = 6 + 3 * (kDeepLayers - 1) + 4;
            break;
        default:
            info.type = "obelisco";
            info.name = "Obelisk of " + pick(kObeliskNames, 6, h);
            site->lore = L == 1 ? 3 + static_cast<int>((h >> 7) % 4u) : base + static_cast<int>((h >> 7) % 2u);
            break;
    }
    const int ex = (site->rot & 1) ? site->hz : site->hx, ez = (site->rot & 1) ? site->hx : site->hz;
    info.center = core::Vec3{static_cast<float>(cx) + 0.5f, static_cast<float>(site->y), static_cast<float>(cz) + 0.5f};
    info.min = core::Vec3{static_cast<float>(cx - ex), static_cast<float>(site->y - 2), static_cast<float>(cz - ez)};
    info.max = core::Vec3{static_cast<float>(cx + ex + 1), static_cast<float>(site->y + std::max(site->h, 12)), static_cast<float>(cz + ez + 1)};
    buildDeepSite(*site, nullptr, &info.points);
    // Capas hondas: guarniciones y jefes de la capa (la guarida tiene los suyos).
    if (L > 1 && kind != kLair) {
        int i = 0;
        for (StructurePoint& p : info.points) {
            if (p.kind == "spawner") p.tag = th.species[(i++ + static_cast<int>(h % 3u)) % 3];
            else if (p.kind == "jefe") p.tag = th.boss;
        }
    }
    return site;
}

// Piedra de Ascenso bajo una sima (capa 1, a la superficie) o Piedra de Subida bajo el pozo de la capa de
// arriba (capas 2..8, a esa capa), con la Escalera de los Delvers en la pared del pozo.
std::shared_ptr<const Generator::DeepSite> Generator::planAscension(const Sinkhole& s) const {
    const int L = deep_layer_;
    const DeepTheme& th = themeOf(L);
    const int lake = std::max(th.sea, kDeepSea);
    auto site = std::make_shared<DeepSite>();
    site->kind = kAscension;
    site->seed = s.id;
    const int axis = static_cast<int>(s.id % 4u);
    static constexpr int kAx[4] = {0, 1, 0, -1}, kAz[4] = {1, 0, -1, 0};
    static constexpr int kRot[4] = {0, 3, 2, 1};  // rot que lleva el +z local a esa direccion (Sink::toWorld)
    site->rot = kRot[axis];
    site->x = s.x + kAx[axis] * 21;
    site->z = s.z + kAz[axis] * 21;
    site->y = lake + 1;
    site->h = 0;
    site->depth = std::max(28, lake - 12);
    site->tag = L == 1 ? std::string("superficie") : layerDim(L - 1);
    StructureInfo& info = site->info;
    if (L == 1) {
        info.id = (std::uint64_t{1} << 50) | (std::uint64_t{1} << 49) | ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.x)) & 0xFFFFFFu) << 24) |
                  (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.z)) & 0xFFFFFFu);
    } else {
        info.id = (std::uint64_t{1} << 50) | (std::uint64_t{1} << 48) | ((static_cast<std::uint64_t>(L - 1) & 7u) << 40) |
                  ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.x)) & 0xFFFFFu) << 20) |
                  (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.z)) & 0xFFFFFu);
    }
    info.type = "portal";
    info.name = L == 1 ? "Ascension Stone" : "Rising Stone";
    info.level = 6 + 3 * (L - 1);
    info.seed = s.id;
    info.dimension = layerDim(L);
    info.center = core::Vec3{static_cast<float>(site->x) + 0.5f, static_cast<float>(site->y), static_cast<float>(site->z) + 0.5f};
    const int lo_x = std::min(site->x, site->x + kAx[axis] * 20) - 4, hi_x = std::max(site->x, site->x + kAx[axis] * 20) + 5;
    const int lo_z = std::min(site->z, site->z + kAz[axis] * 20) - 4, hi_z = std::max(site->z, site->z + kAz[axis] * 20) + 5;
    info.min = core::Vec3{static_cast<float>(lo_x), static_cast<float>(site->y - site->depth), static_cast<float>(lo_z)};
    info.max = core::Vec3{static_cast<float>(hi_x), static_cast<float>(site->y + 8), static_cast<float>(hi_z)};
    buildDeepSite(*site, nullptr, &info.points);
    StructurePoint arrival;  // arriba del pozo de la caida: el juego deja ahi al jugador al bajar
    arrival.kind = "llegada";
    arrival.tag = L == 1 ? "profundo" : "abajo";
    arrival.position = core::Vec3{static_cast<float>(s.x) + 0.5f, static_cast<float>(kDeepArrival), static_cast<float>(s.z) + 0.5f};
    info.points.push_back(arrival);
    if (L > 1) {
        // Arriba de la Escalera de los Delvers: el paso a la capa de arriba (se entra andando).
        const StairEnd e = stairEnd(s, lake + 1);
        StructurePoint up;
        up.kind = "portal";
        up.tag = layerDim(L - 1);
        up.position = core::Vec3{e.x, static_cast<float>(e.y), e.z};
        up.direction = core::Vec3{0.0f, 0.0f, 1.0f};
        info.points.push_back(up);
        info.max.y = static_cast<float>(kWorldHeight - 4);  // que se vea desde arriba del pozo
    }
    return site;
}

// Agujero del suelo de esta capa hacia la de abajo (sima_honda): estanque del fondo (portal), llegada en el
// borde (al subir desde abajo) y los pilares con la tablilla.
std::shared_ptr<const Generator::DeepSite> Generator::planShaftHole(const Sinkhole& s) const {
    const int L = deep_layer_;
    auto site = std::make_shared<DeepSite>();
    site->kind = kShaftHole;
    site->seed = s.id;
    site->x = s.x;
    site->z = s.z;
    site->y = s.top + 1;
    site->h = 0;
    site->r = s.r;
    site->phase = s.phase;
    site->next = L + 1;
    StructureInfo& info = site->info;
    info.id = (std::uint64_t{1} << 50) | (std::uint64_t{1} << 48) | (std::uint64_t{1} << 43) | ((static_cast<std::uint64_t>(L - 1) & 7u) << 40) |
              ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.x)) & 0xFFFFFu) << 20) |
              (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.z)) & 0xFFFFFu);
    info.type = "sima_honda";
    info.name = std::string("The ") + kDropNames[s.id % 8u] + " Drop";
    info.center = core::Vec3{static_cast<float>(s.x) + 0.5f, static_cast<float>(s.top), static_cast<float>(s.z) + 0.5f};
    const float e = s.r + 5.0f;  // como sinkholeInfo: el juego saca el radio del pozo del ancho
    info.min = core::Vec3{static_cast<float>(s.x) - e, 1.0f, static_cast<float>(s.z) - e};
    info.max = core::Vec3{static_cast<float>(s.x) + e + 1.0f, static_cast<float>(s.top + 8), static_cast<float>(s.z) + e + 1.0f};
    info.level = 6 + 3 * (L - 1);
    info.seed = s.id;
    info.dimension = layerDim(L);
    StructurePoint portal;
    portal.kind = "portal";
    portal.tag = layerDim(L + 1);
    portal.position = core::Vec3{static_cast<float>(s.x) + 0.5f, static_cast<float>(kShaftPool), static_cast<float>(s.z) + 0.5f};
    info.points.push_back(portal);
    StructurePoint up;  // al subir desde la capa de abajo: en el borde, entre los dos primeros pilares
    up.kind = "llegada";
    up.tag = "arriba";
    const float a = s.phase + kPi / 6.0f;
    up.position = core::Vec3{static_cast<float>(s.x) + 0.5f + std::cos(a) * (s.r + 5.5f), static_cast<float>(s.top + 1),
                             static_cast<float>(s.z) + 0.5f + std::sin(a) * (s.r + 5.5f)};
    info.points.push_back(up);
    buildDeepSite(*site, nullptr, &info.points);
    return site;
}

std::vector<std::shared_ptr<const Generator::DeepSite>> Generator::deepRegion(int rx, int rz) const {
    if (!deep_cache_ || profile_ != VoxelGenerator::Deep) return {};
    const std::int64_t key = cellKey(rx, rz);
    {
        std::lock_guard<std::mutex> lock(deep_cache_->mutex);
        const auto it = deep_cache_->regions.find(key);
        if (it != deep_cache_->regions.end()) return it->second;
    }
    std::vector<std::shared_ptr<const DeepSite>> list;
    if (auto site = planDeepSite(rx, rz)) list.push_back(std::move(site));
    const int x0 = rx * kDeepRegion, z0 = rz * kDeepRegion;
    const auto inside = [&](const Sinkhole& s) { return s.valid && s.x >= x0 && s.x < x0 + kDeepRegion && s.z >= z0 && s.z < z0 + kDeepRegion; };
    const int L = deep_layer_;
    if (L == 1) {
        // Las Piedras de Ascenso de las caidas cuyo centro cae en la region.
        for (int gz = floorDiv(z0, kSinkCell); gz <= floorDiv(z0 + kDeepRegion - 1, kSinkCell); ++gz)
            for (int gx = floorDiv(x0, kSinkCell); gx <= floorDiv(x0 + kDeepRegion - 1, kSinkCell); ++gx) {
                const Sinkhole s = sinkhole(gx, gz);
                if (inside(s)) list.push_back(planAscension(s));
            }
    }
    // Pozos de la frontera de arriba (Piedra de Subida y escalera) y agujeros del suelo de esta capa.
    for (int b = L - 1; b <= L; ++b) {
        if (b < 1 || b >= kDeepLayers) continue;
        const int shift = (b & 1) ? kShaftCell / 2 : 0;
        for (int gz = floorDiv(z0 - shift, kShaftCell); gz <= floorDiv(z0 + kDeepRegion - 1 - shift, kShaftCell); ++gz)
            for (int gx = floorDiv(x0 - shift, kShaftCell); gx <= floorDiv(x0 + kDeepRegion - 1 - shift, kShaftCell); ++gx) {
                const Sinkhole s = layerShaft(b, gx, gz);
                if (!inside(s)) continue;
                list.push_back(b == L ? planShaftHole(s) : planAscension(s));
            }
    }
    std::lock_guard<std::mutex> lock(deep_cache_->mutex);
    if (deep_cache_->regions.size() > 8192) deep_cache_->regions.clear();
    deep_cache_->regions.emplace(key, list);
    return list;
}

void Generator::buildDeepSite(const DeepSite& site, ChunkData* chunk, std::vector<StructurePoint>* points) const {
    Sink s;
    s.chunk = chunk;
    s.pts = points;
    s.ax = site.x;
    s.ay = site.y;
    s.az = site.z;
    s.rot = site.rot;
    s.seed = site.seed;
    // Zona vaciada (cupula baja; cuadrada en las ciudades) con su suelo y cimientos (coordenadas del mundo, sin girar).
    if (chunk != nullptr && site.h > 0) {
        const BlockId rock = themeOf(deep_layer_).rock == block::Basalt ? block::Basalt : block::UmbralStone;
        const int found = site.kind == kCity || site.kind == kLair ? 22 : 9;
        const int ex = ((site.rot & 1) ? site.hz : site.hx) + 2, ez = ((site.rot & 1) ? site.hx : site.hz) + 2;
        const int x0 = chunk->cx * kChunkSize, z0 = chunk->cz * kChunkSize;
        for (int z = std::max(z0, site.z - ez); z <= std::min(z0 + kChunkSize - 1, site.z + ez); ++z) {
            for (int x = std::max(x0, site.x - ex); x <= std::min(x0 + kChunkSize - 1, site.x + ex); ++x) {
                const float dx = static_cast<float>(x - site.x) / static_cast<float>(ex), dz = static_cast<float>(z - site.z) / static_cast<float>(ez);
                const float e = site.square ? std::max(dx * dx, dz * dz) : dx * dx + dz * dz;
                if (e > 1.35f) continue;
                const int lx = x - x0, lz = z - z0;
                const auto at = [&](int y) -> BlockId& { return chunk->blocks[static_cast<std::size_t>(cellIndex(lx, y, lz))]; };
                for (int y = std::max(1, site.y - found); y <= site.y - 2; ++y)
                    if (!solidId(at(y))) at(y) = rock;
                if (e > 1.0f) continue;
                BlockId& ground = at(site.y - 1);
                if (site.floor != block::Air) ground = site.floor;
                else if (!solidId(ground)) ground = rock;
                const int top = site.y + site.h - static_cast<int>(e * e * static_cast<float>(site.h) * 0.45f);
                for (int y = site.y; y < std::min(top, kWorldHeight - 4); ++y) at(y) = block::Air;
            }
        }
    }
    switch (site.kind) {
        case kVillage: buildVillage(s); break;
        case kHermit: buildHermit(s, site.lore); break;
        case kHall: buildHall(s, site.lore); break;
        case kTemple: buildTemple(s, site.pattern, site.colors); break;
        case kSanctum: buildSanctum(s, site.lore); break;
        case kColony: buildColony(s, site.lore); break;
        case kObelisk: buildObelisk(s, site.lore); break;
        case kAscension: buildAscension(s, site.tag, site.depth); break;
        case kCity: buildCity(s, site.ruin, site.capital, site.lore); break;
        case kLair: buildLair(s); break;
        case kShaftHole: buildShaftRim(s, site.r, site.phase, site.next); break;
        default: break;
    }
}
// --- Consultas ------------------------------------------------------------------------

namespace {
bool touches(const StructureInfo& info, float x, float y, float z, float radius) {
    const float dx = std::max({info.min.x - x, 0.0f, x - info.max.x});
    const float dy = std::max({info.min.y - 4.0f - y, 0.0f, y - info.max.y - 4.0f});
    const float dz = std::max({info.min.z - z, 0.0f, z - info.max.z});
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}
}  // namespace

std::vector<StructureInfo> Generator::deepStructuresNear(float x, float y, float z, float radius) const {
    std::vector<StructureInfo> out;
    if (!deep_cache_) return out;
    radius = std::min(radius, 4000.0f);
    const int ix = static_cast<int>(std::floor(x)), iz = static_cast<int>(std::floor(z)), ir = static_cast<int>(std::ceil(radius));
    if (profile_ == VoxelGenerator::Adventure) {
        if (!immense_caves_) return out;
        for (int gz = floorDiv(iz - ir - 32, kSinkCell); gz <= floorDiv(iz + ir + 32, kSinkCell); ++gz)
            for (int gx = floorDiv(ix - ir - 32, kSinkCell); gx <= floorDiv(ix + ir + 32, kSinkCell); ++gx) {
                const Sinkhole s = sinkhole(gx, gz);
                if (!s.valid) continue;
                StructureInfo info = sinkholeInfo(s, seed_);
                if (touches(info, x, y, z, radius)) out.push_back(std::move(info));
            }
        return out;
    }
    if (profile_ != VoxelGenerator::Deep) return out;
    for (int rz = floorDiv(iz - ir - 48, kDeepRegion); rz <= floorDiv(iz + ir + 48, kDeepRegion); ++rz)
        for (int rx = floorDiv(ix - ir - 48, kDeepRegion); rx <= floorDiv(ix + ir + 48, kDeepRegion); ++rx)
            for (const auto& site : deepRegion(rx, rz))
                if (touches(site->info, x, y, z, radius)) out.push_back(site->info);
    return out;
}

bool Generator::deepStructureAt(float x, float y, float z, StructureInfo& out) const {
    for (StructureInfo& s : deepStructuresNear(x, y, z, 0.0f)) {
        out = std::move(s);
        out.room = -1;
        return true;
    }
    return false;
}

// --- Reino Profundo: el chunk ------------------------------------------------------------

namespace {

// Punta de cristal inclinada (como en la dimension de cristal).
void deepSpire(ChunkData& chunk, float bx, float by, float bz, float dx, float dy, float dz, float length, float r0, BlockId id) {
    const int x0 = chunk.cx * kChunkSize, z0 = chunk.cz * kChunkSize;
    for (float s = 0.0f; s <= length; s += 0.5f) {
        const float k = s / length;
        const float radius = r0 * (1.0f - k) + 0.45f;
        const float px = bx + dx * s, py = by + dy * s, pz = bz + dz * s;
        const int ir = static_cast<int>(std::ceil(radius));
        for (int oy = -ir; oy <= ir; ++oy)
            for (int oz = -ir; oz <= ir; ++oz)
                for (int ox = -ir; ox <= ir; ++ox) {
                    if (static_cast<float>(ox * ox + oy * oy + oz * oz) > radius * radius) continue;
                    const int wx = static_cast<int>(std::floor(px)) + ox, wy = static_cast<int>(std::floor(py)) + oy,
                              wz = static_cast<int>(std::floor(pz)) + oz;
                    const int lx = wx - x0, lz = wz - z0;
                    if (lx < 0 || lz < 0 || lx >= kChunkSize || lz >= kChunkSize || wy < 1 || wy >= kWorldHeight - 3) continue;
                    chunk.blocks[static_cast<std::size_t>(cellIndex(lx, wy, lz))] = id;
                }
    }
}

// Mena honda en la roca: bolsas de 4 x 4 x 4 (hash) con la mitad de sus celdas. Cuanto mas honda la
// capa, mas ricas (DeepTheme::ore) y desde la 5, adamantita.
BlockId deepOre(int x, int y, int z, std::uint32_t seed, const DeepTheme& th, int layer) {
    const std::uint32_t h = hash3(x >> 2, y >> 2, z >> 2, seed);
    const float p = static_cast<float>(h & 0xFFFF) / 65535.0f;
    const float r = th.ore;
    const float a = layer >= 5 ? 0.0025f * static_cast<float>(layer - 4) : 0.0f;
    if (p > 0.11f + (r - 1.0f) * 0.04f + a || rand01(x, y, z, seed + 3u) > 0.45f) return th.rock;
    if (p < a) return block::AdamantOre;
    const int deeper = 30 * (layer - 1);
    if (p < a + 0.008f * r && y < 130 + deeper) return block::MithrilOre;
    if (p < a + 0.020f * r && y < 70 + deeper) return block::DiamondOre;
    if (p < a + 0.035f * r) return block::GoldOre;
    if (p < a + 0.055f * r) return block::CrystalOre;
    if (p < a + 0.072f * r) return block::ArcaniteOre;
    if (p < a + 0.095f * r) return block::IronOre;
    return block::CoalOre;
}

}  // namespace

void Generator::generateDeep(ChunkData& chunk) const {
    const int L = deep_layer_;
    const DeepTheme& th = themeOf(L);
    const int x0 = chunk.cx * kChunkSize, z0 = chunk.cz * kChunkSize;
    const auto seed = static_cast<std::uint32_t>(seed_) + 5300u + (L > 1 ? static_cast<std::uint32_t>(L - 1) * 7919u : 0u);
    chunk.grass_tint.fill(0xFFF);
    chunk.foliage_tint.fill(0xFFF);
    // Lo que baja del techo (bajo una sima de la superficie, o el pozo de la capa de arriba) y el agujero del
    // suelo hacia la capa de abajo. Las celdas de los pozos estan alineadas con los chunks: uno de cada.
    const Sinkhole fall = L == 1 ? sinkhole(floorDiv(x0, kSinkCell), floorDiv(z0, kSinkCell)) : layerShaftAt(L - 1, x0, z0);
    const Sinkhole hole = L < kDeepLayers ? layerShaftAt(L, x0, z0) : Sinkhole{};
    const int sea = th.sea, lake = std::max(th.sea, kDeepSea);
    const bool lava_sea = th.liquid == block::Lava;
    std::array<DeepColumn, kChunkSize * kChunkSize> cols{};
    for (int z = 0; z < kChunkSize; ++z)
        for (int x = 0; x < kChunkSize; ++x)
            cols[static_cast<std::size_t>(x + z * kChunkSize)] = deepColumn(x0 + x, z0 + z, fall.valid ? &fall : nullptr, hole.valid ? &hole : nullptr);

    // Arcos y rocas colgadas en las dos capas (ruido 3D en una rejilla de 4 m).
    constexpr int kStep = 4, kG = kChunkSize / kStep + 1, kGy = kWorldHeight / kStep + 1;
    const float ox = layerOffX(L), oz = layerOffZ(L);
    std::vector<float> rock(static_cast<std::size_t>(kG * kG * kGy));
    for (int iy = 0; iy < kGy; ++iy)
        for (int iz = 0; iz < kG; ++iz)
            for (int ix = 0; ix < kG; ++ix)
                rock[static_cast<std::size_t>(ix + iz * kG + iy * kG * kG)] =
                    noise3((static_cast<float>(x0 + ix * kStep) + ox) / 42.0f, static_cast<float>(iy * kStep) / 30.0f,
                           (static_cast<float>(z0 + iz * kStep) + oz) / 42.0f, 1150) - 0.5f;
    const auto rockAt = [&](int x, int y, int z) {
        const int ix = x / kStep, iy = std::min(y / kStep, kGy - 2), iz = z / kStep;
        const float fx = static_cast<float>(x % kStep) / kStep, fy = static_cast<float>(y - iy * kStep) / kStep,
                    fz = static_cast<float>(z % kStep) / kStep;
        const auto G = [&](int a, int b, int c) { return rock[static_cast<std::size_t>(a + c * kG + b * kG * kG)]; };
        const float c00 = lerp(G(ix, iy, iz), G(ix + 1, iy, iz), fx), c10 = lerp(G(ix, iy, iz + 1), G(ix + 1, iy, iz + 1), fx);
        const float c01 = lerp(G(ix, iy + 1, iz), G(ix + 1, iy + 1, iz), fx), c11 = lerp(G(ix, iy + 1, iz + 1), G(ix + 1, iy + 1, iz + 1), fx);
        return lerp(lerp(c00, c10, fz), lerp(c01, c11, fz), fy);
    };

    const BlockId kLowTop[4] = {th.drip, block::GlowMoss, block::DuskStone, block::Basalt};
    const BlockId kLowSub[4] = {th.drip, block::DuskSoil, block::DuskStone, block::Basalt};
    const BlockId kUpTop[3] = {th.drip, block::GlowMoss, block::DuskStone};
    const BlockId kUpSub[3] = {th.drip, block::DuskSoil, block::DuskStone};
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            const DeepColumn& c = cols[static_cast<std::size_t>(x + z * kChunkSize)];
            const int wx = x0 + x, wz = z0 + z;
            const bool sea_floor = c.lf < sea - 1;
            const bool wall = fall.valid && !c.well && c.well_d < fall.r + 2.6f;  // pared del pozo de la caida
            const bool basin = fall.valid && c.well_d < 30.0f;                    // lago de la caida: siempre agua
            for (int y = 0; y < kWorldHeight; ++y) {
                BlockId id = block::Air;
                bool solid;
                if (c.well) solid = y <= c.lf || y >= kWorldHeight - 3;
                else solid = y <= c.lf || (y >= c.lc && y <= c.uf && !c.chasm) || y >= c.uc;
                if (!solid && !c.well) {
                    const bool low = y > c.lf + 5 && y < c.lc - 5, high = y > c.uf + 5 && y < c.uc - 5;
                    if ((low || high) && rockAt(x, y, z) > th.arches) solid = true;
                }
                if (y == 0 || (y < 3 && rand01(wx, y, wz, seed) < 0.5f)) {
                    id = block::Bedrock;
                } else if (solid) {
                    if (y <= c.lf && c.lf - y < 4) {
                        const int depth = c.lf - y;
                        if (sea_floor && lava_sea) {
                            id = depth == 0 && rand01(wx, 1, wz, seed) < 0.3f ? block::Magma : block::Basalt;
                        } else if (sea_floor) {
                            id = depth == 0 && rand01(wx, 1, wz, seed) < 0.6f ? block::SeaSilt : block::Gravel;
                        } else {
                            id = depth == 0 ? kLowTop[c.lower] : kLowSub[c.lower];
                            if (c.lower == 3 && depth == 0 && rand01(wx, 2, wz, seed) < 0.08f) id = block::Magma;
                        }
                    } else if (!c.chasm && y <= c.uf && c.uf - y < 3 && y >= c.lc) {
                        id = c.uf == y ? kUpTop[c.upper] : kUpSub[c.upper];
                    } else if (y >= kWorldHeight - 3) {
                        id = th.rock;
                    } else {
                        id = deepOre(wx, y, wz, seed, th, L);
                        if (c.lower == 3 && y < c.lc && id == th.rock && rand01(wx, y, wz, seed + 9u) < 0.5f) id = block::Basalt;
                    }
                    if (wall && y > c.lf && y % 16 == 8 && c.well_d < fall.r + 1.4f) id = block::GlyphStone;
                } else if (y > c.lf && (y < c.lc || c.chasm || c.well)) {
                    if (basin ? y < lake : y < sea) id = basin ? block::Water : th.liquid;
                }
                chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))] = id;
            }
            const auto get = [&](int y) { return chunk.get(x, y, z); };
            const auto put = [&](int y, BlockId id) {
                if (y > 0 && y < kWorldHeight - 1) chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))] = id;
            };
            // El agujero del suelo (capas 1..7): abierto hasta el estanque del fondo (y 2..9, fondo de glifos),
            // pared maciza con anillos de glifos y el brocal de piedra tallada en la plataforma.
            if (hole.valid && c.hole_d < hole.r + 4.5f) {
                const float d = c.hole_d;
                if (c.hole) {
                    for (int y = 0; y <= c.lf; ++y) {
                        BlockId id = block::Air;
                        if (y == 0) id = block::Bedrock;
                        else if (y == 1) id = d > hole.r - 1.6f ? block::GlyphStone : block::Obsidian;
                        else if (y < kShaftPool) id = block::Water;
                        chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))] = id;
                    }
                    continue;
                }
                if (d < hole.r + 2.2f) {
                    for (int y = 1; y < c.lf; ++y) {
                        BlockId& b = chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))];
                        if (y % 14 == 6 && d < hole.r + 1.0f) b = block::GlyphStone;
                        else if (!solidId(b)) b = th.rock;
                    }
                }
                put(c.lf, d < hole.r + 2.2f || rand01(wx, 3, wz, seed) < 0.7f ? block::CarvedStone : block::Cobblestone);
                if (blockDef(get(c.lf + 1)).replaceable) put(c.lf + 1, block::Air);
            }
            if (c.well) continue;
            // Rios de lava de las brasas.
            if (c.river > 0.2f && get(c.lf + 1) == block::Air) {
                if (c.river > 0.35f) {
                    put(c.lf, block::Lava);
                    if (c.river > 0.65f) put(c.lf - 1, block::Lava);
                } else {
                    put(c.lf, block::Magma);
                }
            }
            // Plantas del suelo de abajo (o del fondo del mar).
            const float r = rand01(wx, 7, wz, seed);
            if (get(c.lf + 1) == block::Water) {
                if (r < 0.015f) put(c.lf + 1, block::SeaGlow);
                else if (r < 0.07f) put(c.lf + 1, block::SeaGrass);
            } else if (get(c.lf + 1) == block::Air && solidId(get(c.lf)) && !(hole.valid && c.hole_d < hole.r + 4.5f)) {
                BlockId plant = block::Air;
                switch (c.lower) {
                    case 0: plant = r < 0.04f ? block::Stalagmite : (r < 0.05f ? block::BrownMushroom : block::Air); break;
                    case 1: plant = r < 0.06f ? block::GlowMushroom : (r < 0.08f ? block::RedMushroom : (r < 0.28f ? block::GlowGrass : block::Air)); break;
                    case 2: plant = r < 0.05f ? block::CrystalSprout : block::Air; break;
                    default: plant = r < 0.03f && get(c.lf) == block::Basalt ? block::EmberGrass : block::Air; break;
                }
                if (plant != block::Air) put(c.lf + 1, plant);
            }
            // Lo que cuelga del techo de abajo y del de arriba.
            const auto hang = [&](int ceiling, float roll, float vines, float stal, float roots) {
                if (ceiling - 1 <= 0 || get(ceiling - 1) != block::Air || !solidId(get(ceiling))) return;
                if (roll < vines) {
                    const int len = 1 + static_cast<int>(rand01(wx, ceiling, wz, seed + 4u) * 5.0f);
                    for (int k = 1; k <= len && get(ceiling - k) == block::Air; ++k) put(ceiling - k, block::GlowVines);
                } else if (roll < vines + stal) {
                    put(ceiling - 1, block::Stalactite);
                } else if (roll < vines + stal + roots) {
                    const int len = 1 + static_cast<int>(rand01(wx, ceiling, wz, seed + 5u) * 3.0f);
                    for (int k = 1; k <= len && get(ceiling - k) == block::Air; ++k) put(ceiling - k, block::Duskroot);
                }
            };
            const float rh = rand01(wx, 8, wz, seed);
            if (!c.chasm) hang(c.lc, rh, 0.05f, 0.05f, 0.03f);
            static constexpr float kVines[3] = {0.03f, 0.10f, 0.02f}, kStal[3] = {0.12f, 0.02f, 0.03f}, kRoots[3] = {0.03f, 0.03f, 0.06f};
            hang(c.uc, rand01(wx, 9, wz, seed), kVines[c.upper], kStal[c.upper], kRoots[c.upper]);
            // Plantas del suelo de arriba.
            if (!c.chasm && get(c.uf + 1) == block::Air && solidId(get(c.uf))) {
                const float ru = rand01(wx, 10, wz, seed);
                BlockId plant = block::Air;
                if (c.upper == 0) plant = ru < 0.09f ? block::Stalagmite : block::Air;
                else if (c.upper == 1) plant = ru < 0.06f ? block::GlowMushroom : (ru < 0.32f ? block::GlowGrass : block::Air);
                else plant = ru < 0.06f ? block::CrystalSprout : (ru < 0.07f ? block::GlowMushroom : block::Air);
                if (plant != block::Air) put(c.uf + 1, plant);
            }
        }
    }

    // --- Rasgos que cruzan chunks: hongos gigantes, racimos de cristal y columnas ---
    const float fall_keep = L > 1 ? 38.0f : 30.0f;
    const auto groundOf = [&](const DeepColumn& c, int tx, int tz, bool upper, int& biome) {
        if (fall.valid && sinkDistance(fall, static_cast<float>(tx), static_cast<float>(tz)) < fall_keep) return -1;
        if (hole.valid && sinkDistance(hole, static_cast<float>(tx), static_cast<float>(tz)) < hole.r + 26.0f) return -1;
        if (upper) {
            biome = c.upper;
            return c.chasm ? -1 : c.uf;
        }
        biome = c.lower;
        return c.lf < std::max(sea, 1) ? -1 : c.lf;
    };
    for (int layer = 0; layer < 2; ++layer) {
        const bool upper = layer == 1;
        constexpr int kCell = 14, kReach = 10;
        const std::uint32_t s = seed + 60u + static_cast<std::uint32_t>(layer) * 7u;
        for (int gz = floorDiv(z0 - kReach, kCell); gz <= floorDiv(z0 + kChunkSize + kReach, kCell); ++gz) {
            for (int gx = floorDiv(x0 - kReach, kCell); gx <= floorDiv(x0 + kChunkSize + kReach, kCell); ++gx) {
                if (rand01(gx, 0, gz, s) > 0.45f) continue;
                const int tx = gx * kCell + 2 + static_cast<int>(rand01(gx, 1, gz, s) * (kCell - 4));
                const int tz = gz * kCell + 2 + static_cast<int>(rand01(gx, 2, gz, s) * (kCell - 4));
                const DeepColumn tc = deepColumn(tx, tz, nullptr);
                int biome = 0;
                const int ground = groundOf(tc, tx, tz, upper, biome);
                if (ground < 0) continue;
                const int ceiling = upper ? tc.uc : tc.lc;
                const float size = rand01(gx, 3, gz, s);
                if (biome == 1) {
                    // Hongo gigante luminoso.
                    const int h = 7 + static_cast<int>(size * 13.0f);
                    const float radius = 4.0f + size * 4.0f;
                    if (ground + h + 5 >= ceiling) continue;
                    const BlockId cap = rand01(gx, 6, gz, s) < 0.5f ? block::ShroomCapBlue : block::ShroomCapViolet;
                    for (int i = 0; i < h; ++i)
                        for (int k = 0; k < (h > 12 ? 4 : 1); ++k) {
                            const int sx = tx + (k & 1) - x0, sz = tz + (k >> 1) - z0;
                            if (sx >= 0 && sz >= 0 && sx < kChunkSize && sz < kChunkSize)
                                chunk.blocks[static_cast<std::size_t>(cellIndex(sx, ground + 1 + i, sz))] = block::ShroomStem;
                        }
                    const float cap_h = radius * 0.5f, inner = radius - 1.6f;
                    const int cy = ground + 1 + h, ri = static_cast<int>(std::ceil(radius));
                    for (int dy = -1; dy <= static_cast<int>(std::ceil(cap_h)); ++dy)
                        for (int dz = -ri; dz <= ri; ++dz)
                            for (int dx = -ri; dx <= ri; ++dx) {
                                const int lx = tx + dx - x0, lz = tz + dz - z0;
                                if (lx < 0 || lz < 0 || lx >= kChunkSize || lz >= kChunkSize) continue;
                                const float r2 = static_cast<float>(dx * dx + dz * dz);
                                bool on = false;
                                if (dy < 0) {
                                    on = r2 / (radius * radius) >= 0.72f && r2 <= radius * radius;
                                } else {
                                    const float in_y = static_cast<float>(dy) + 1.2f;
                                    on = r2 / (radius * radius) + static_cast<float>(dy * dy) / (cap_h * cap_h) <= 1.0f &&
                                         r2 / (inner * inner) + in_y * in_y / (cap_h * cap_h) > 1.0f;
                                }
                                if (!on) continue;
                                BlockId& cell = chunk.blocks[static_cast<std::size_t>(cellIndex(lx, cy + dy, lz))];
                                if (cell == block::Air || blockDef(cell).replaceable) cell = cap;
                            }
                } else if (biome == 2) {
                    // Racimo de cristales (y, arriba, tambien colgando del techo). En la capa 8, rosas (el Corazon).
                    static constexpr BlockId kC[4] = {block::CrystalBlue, block::CrystalViolet, block::CrystalPink, block::CrystalGreen};
                    const BlockId id = L == kDeepLayers ? block::CrystalPink : kC[static_cast<int>(rand01(gx, 4, gz, s) * 4.0f) & 3];
                    const int n = 1 + static_cast<int>(rand01(gx, 5, gz, s) * 4.0f);
                    for (int i = 0; i < n; ++i) {
                        const float dx = (rand01(gx, 10 + i, gz, s) - 0.5f) * (i == 0 ? 0.4f : 1.3f);
                        const float dz = (rand01(gx, 20 + i, gz, s) - 0.5f) * (i == 0 ? 0.4f : 1.3f);
                        const float len = std::sqrt(dx * dx + dz * dz + 1.0f);
                        const float length = std::min(static_cast<float>(ceiling - ground - 3),
                                                      (i == 0 ? 7.0f : 3.0f) + rand01(gx, 30 + i, gz, s) * (i == 0 ? 14.0f : 6.0f));
                        if (length < 2.0f) continue;
                        deepSpire(chunk, static_cast<float>(tx) + 0.5f, static_cast<float>(ground), static_cast<float>(tz) + 0.5f, dx / len, 1.0f / len,
                                  dz / len, length, i == 0 ? 1.4f + size : 0.7f, id);
                    }
                    if (upper && rand01(gx, 7, gz, s) < 0.6f) {
                        deepSpire(chunk, static_cast<float>(tx) + 0.5f, static_cast<float>(ceiling), static_cast<float>(tz) + 0.5f, 0.0f, -1.0f, 0.0f,
                                  std::min(18.0f, static_cast<float>(ceiling - ground) * 0.3f), 1.6f, id);
                    }
                } else if (biome == 0 && (upper || rand01(gx, 9, gz, s) < 0.5f) && rand01(gx, 8, gz, s) < 0.35f && ceiling - ground <= 120) {
                    // Columna del suelo al techo (mas ancha en los extremos), con glifos: de goteo, de basalto,
                    // de roca del Hueco o, en la capa 8, raiz viva.
                    const float radius = 1.5f + size * 2.5f;
                    const int ri = static_cast<int>(std::ceil(radius * 2.2f));
                    for (int y = ground + 1; y < ceiling; ++y) {
                        const float a = std::max(0.0f, 1.0f - static_cast<float>(y - ground) / 8.0f);
                        const float b = std::max(0.0f, 1.0f - static_cast<float>(ceiling - y) / 8.0f);
                        const float ry = radius * (1.0f + 1.2f * a * a + 1.2f * b * b);
                        for (int dz = -ri; dz <= ri; ++dz)
                            for (int dx = -ri; dx <= ri; ++dx) {
                                const int lx = tx + dx - x0, lz = tz + dz - z0;
                                if (lx < 0 || lz < 0 || lx >= kChunkSize || lz >= kChunkSize) continue;
                                const float d2 = static_cast<float>(dx * dx + dz * dz);
                                if (d2 > ry * ry) continue;
                                BlockId& cell = chunk.blocks[static_cast<std::size_t>(cellIndex(lx, y, lz))];
                                if (!solidId(cell)) cell = (y - ground) % 19 == 9 && d2 >= ry * ry - 3.0f ? block::GlyphStone : th.column;
                            }
                    }
                }
            }
        }
    }

    // --- Escalera de los Delvers (capas 2..8): espiral por la pared del pozo del techo, del lago al rellano ---
    if (L > 1 && fall.valid) {
        const int base = lake + 1;
        const float total = stairTurns(base);
        const StairEnd end = stairEnd(fall, base);
        const float cxw = static_cast<float>(fall.x) + 0.5f, czw = static_cast<float>(fall.z) + 0.5f;
        for (int z = 0; z < kChunkSize; ++z) {
            for (int x = 0; x < kChunkSize; ++x) {
                const float wx = static_cast<float>(x0 + x) + 0.5f, wz = static_cast<float>(z0 + z) + 0.5f;
                const auto put = [&](int y, BlockId id) {
                    if (y > 0 && y < kWorldHeight - 3) chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))] = id;
                };
                const float dx = wx - cxw, dz = wz - czw;
                const float d = std::hypot(dx, dz);
                if (d >= fall.r + 0.9f && d <= fall.r + 3.3f) {
                    float t = (std::atan2(dz, dx) - fall.phase) / (2.0f * kPi);
                    t -= std::floor(t);
                    for (int n = 0; static_cast<float>(n) + t <= total; ++n) {
                        const int ys = base + static_cast<int>((static_cast<float>(n) + t) * static_cast<float>(kStairTurn));
                        put(ys - 1, static_cast<int>(t * 48.0f) % 8 == 0 && d > fall.r + 2.4f ? block::GlyphStone : block::DelverBricks);
                        for (int k = 0; k < 4; ++k) put(ys + k, block::Air);
                    }
                }
                // Rellano de arriba (encima de los ultimos escalones; el paso a la capa de arriba esta en su centro).
                const float le = std::hypot(wx - end.x, wz - end.z);
                if (le < 2.6f) {
                    put(end.y - 1, le < 0.8f ? block::GlyphStone : block::DelverBricks);
                    for (int k = 0; k < 4; ++k) put(end.y + k, block::Air);
                }
            }
        }
    }

    // --- Estructuras (aldeas ocultas, salas, templos, santuarios, ciudades...), Piedras y agujeros ---
    for (int rz = floorDiv(z0 - 64, kDeepRegion); rz <= floorDiv(z0 + kChunkSize + 64, kDeepRegion); ++rz)
        for (int rx = floorDiv(x0 - 64, kDeepRegion); rx <= floorDiv(x0 + kChunkSize + 64, kDeepRegion); ++rx)
            for (const auto& site : deepRegion(rx, rz)) {
                const StructureInfo& i = site->info;
                if (i.max.x + 8 < static_cast<float>(x0) || i.min.x - 8 > static_cast<float>(x0 + kChunkSize) ||
                    i.max.z + 8 < static_cast<float>(z0) || i.min.z - 8 > static_cast<float>(z0 + kChunkSize))
                    continue;
                buildDeepSite(*site, &chunk, nullptr);
            }
}
}  // namespace cramion::voxel

