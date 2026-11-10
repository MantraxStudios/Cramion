#ifndef CRAMION_CORE_VOXEL_STRUCTURES_H
#define CRAMION_CORE_VOXEL_STRUCTURES_H

// Estructuras del mundo de bloques: aldeas, ciudades (la capital en el origen),
// torres de mago, ruinas, portales y mazmorras. Se generan DENTRO del
// generador de chunks:
//
//   - Planificacion por region (rejilla de kRegionSize metros con el origen en
//     el centro de la region 0,0): una funcion pura de (semilla, region) que
//     decide que hay, donde, su trazado (calles, casas, salas...) y construye
//     cada pieza (casa, muralla, sala) como un plano de bloques pequeno. Cada
//     estructura cabe entera en su region, asi que un chunk solo mira la suya.
//   - Estampado por chunk: al generar un chunk se aplana el terreno bajo las
//     aldeas y ciudades (con rampas suaves), se pintan las calles y se copian
//     las partes de las piezas que caen dentro. El resultado no depende del
//     orden de carga ni del hilo: las estructuras cruzan los bordes de los
//     chunks sin cortes.
//   - Los planes se guardan en una cache (con candado) para no repetirlos en
//     cada chunk de la region.

#include "VoxelInternal.h"

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::voxel {

// Lado de la region de planificacion (m). Las estructuras caben dentro.
inline constexpr int kRegionSize = 384;
// Celda de un plano que no cambia lo que hay (el terreno sigue).
inline constexpr BlockId kKeep = 0xFFFF;  // IDS16: antes 255 (el ultimo id de 8 bits); ahora fuera de block::Count

// Plano de una pieza en coordenadas locales (x ancho, y alto, z fondo; la
// fachada mira a -z). Las celdas kKeep no tocan el mundo.
struct Blueprint {
    int w = 0, h = 0, d = 0;
    std::vector<BlockId> cells;

    Blueprint() = default;
    Blueprint(int w_, int h_, int d_) : w(w_), h(h_), d(d_), cells(static_cast<std::size_t>(w_ * h_ * d_), kKeep) {}
    bool inside(int x, int y, int z) const { return x >= 0 && y >= 0 && z >= 0 && x < w && y < h && z < d; }
    std::size_t index(int x, int y, int z) const { return static_cast<std::size_t>(x + z * w + y * w * d); }
    BlockId get(int x, int y, int z) const { return inside(x, y, z) ? cells[index(x, y, z)] : kKeep; }
    void set(int x, int y, int z, BlockId id) {
        if (inside(x, y, z)) cells[index(x, y, z)] = id;
    }
    // Caja llena (extremos incluidos).
    void box(int x0, int y0, int z0, int x1, int y1, int z1, BlockId id) {
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) set(x, y, z, id);
    }
};

// Un punto del plano (local) para el juego.
struct LocalPoint {
    std::string kind;
    float x = 0, y = 0, z = 0;    // local (bloques; x y z en el centro de la celda)
    int dir = 0;                  // 0 -z (fachada), 1 +x, 2 +z, 3 -x
    std::string tag;
};

// Una pieza colocada: su plano girado (rot * 90 grados) con la esquina minima
// en (x, y, z) del mundo.
struct Piece {
    std::shared_ptr<const Blueprint> plan;
    int x = 0, y = 0, z = 0;
    int rot = 0;
    // Caja en el mundo (incluida).
    int min_x = 0, min_z = 0, max_x = 0, max_z = 0;
    int sizeX() const { return (rot & 1) ? plan->d : plan->w; }
    int sizeZ() const { return (rot & 1) ? plan->w : plan->d; }
};

// Aplanado del terreno: dentro de `inner` a la altura `base`; hasta `outer`
// una rampa suave con el terreno natural. Circulo o cuadrado (redondeado).
struct Terraform {
    int cx = 0, cz = 0;
    float inner = 0, outer = 0;
    int base = 64;
    bool square = false;
    BlockId surface = block::Grass;  // suelo nuevo (si el natural no vale)
    BlockId under = block::Dirt;
    float decor = 0.0f;  // hierba y flores sueltas (0..1)
};

// Calle o camino: segmento con ancho. Se pinta sobre el suelo.
struct Road {
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    float half_width = 1.5f;
    BlockId surface = block::DirtPath;
    BlockId alt = block::Gravel;  // mezcla irregular (0 = sin mezcla)
    float alt_chance = 0.2f;
};

// CIUDADES-GRANDES: tramo de muralla en cualquier angulo (las murallas redondas de las
// metropolis y castillos). Se estampa por columna sobre el suelo de esa columna: macizo de
// 2 * half de grueso y `height` de alto, adarve arriba y almenas por fuera (el lado
// izquierdo de (x0, z0) -> (x1, z1) es el de fuera).
struct WallLine {
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    int height = 10;
    float half = 2.0f;
};

struct Structure {
    StructureInfo info;
    int min_x = 0, min_y = 0, min_z = 0, max_x = 0, max_y = 0, max_z = 0;
    float clear_x = 0, clear_z = 0, clear_radius = 0;  // sin arboles (circulo)
    std::vector<Terraform> terraforms;
    std::vector<Road> roads;
    std::vector<Piece> pieces;
    std::vector<WallLine> walls;  // CIUDADES-GRANDES: murallas en angulo (metropolis, castillos)
    // CIUDADES-GRANDES: tapar las cuevas bajo lo aplanado (hasta 8 por debajo del suelo nuevo):
    // si no, los suelos de las casas quedan colgando sobre una cueva o un barranco.
    bool seal = false;
    // CIUDADES-GRANDES: caja de las calles (para no mirar todas en blocksTrees) y entradas
    // para los caminos entre regiones (las metropolis cruzan regiones).
    float road_x0 = 0, road_z0 = 0, road_x1 = 0, road_z1 = 0;
    std::vector<std::array<float, 2>> hubs;
    // Salas de la mazmorra (cajas en el mundo) para structureAt.
    struct Room {
        int x0, y0, z0, x1, y1, z1;
    };
    std::vector<Room> rooms;
    // LADERAS: suelo propio por columna (aldeas en ladera, en vez de aplanar a una
    // sola altura). Ventana de ground_n x ground_n desde (ground_x0, ground_z0):
    // altura del suelo y que hay (GroundKind). Vacio = el aplanado de siempre.
    enum GroundKind : std::uint8_t { GTerrain = 0, GRoad = 1, GStep = 2, GPad = 3, GPlaza = 4 };
    int ground_x0 = 0, ground_z0 = 0, ground_n = 0;
    std::vector<std::int16_t> ground;
    std::vector<std::uint8_t> ground_kind;
    bool groundAt(int x, int z, int& h, std::uint8_t* kind = nullptr) const {
        const int i = x - ground_x0, j = z - ground_z0;
        if (ground.empty() || i < 0 || j < 0 || i >= ground_n || j >= ground_n) return false;
        const std::size_t k = static_cast<std::size_t>(i + j * ground_n);
        h = ground[k];
        if (kind) *kind = ground_kind[k];
        return true;
    }
};

struct RegionPlan {
    int rx = 0, rz = 0;
    std::vector<Structure> structures;
    // CIUDADES-GRANDES: metropolis (de su celda de 3 x 3 regiones) que tocan esta region. La
    // misma para todas las regiones que toca (no se copia).
    std::vector<std::shared_ptr<const Structure>> shared;
    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (const Structure& s : structures) fn(s);
        for (const auto& s : shared) fn(*s);
    }
    // Caminos entre asentamientos (de borde a borde de la region): se pintan
    // fuera de las zonas aplanadas de las estructuras y no cuentan en sus cajas.
    std::vector<Road> roads;
};

class Structures {
public:
    explicit Structures(const Generator& generator, std::string dimension = "superficie");

    // Estampa en el chunk recien generado lo que le toca. `heights` (opcional):
    // altura del suelo natural de cada columna (x + z * 16); el suelo se busca
    // desde un poco por encima (sin ella, desde arriba del todo: las islas
    // flotantes de las tierras salvajes se tomarian por el suelo).
    void stamp(ChunkData& chunk, const std::array<int, kChunkSize * kChunkSize>* heights = nullptr) const;
    // Sin arboles en (x, z): dentro de una aldea, ciudad, torre... `margin` (m):
    // ademas esa franja alrededor de su zona y de sus caminos (0 = lo de siempre).
    bool blocksTrees(int x, int z, float margin = 0.0f) const;
    // Altura del suelo tras aplanar (natural = la del generador).
    int groundHeight(int x, int z, int natural) const;

    // Consultas (coordenadas absolutas).
    std::vector<StructureInfo> nearby(float x, float y, float z, float radius) const;
    bool at(float x, float y, float z, StructureInfo& out) const;
    // El plan de una region (cacheado).
    std::shared_ptr<const RegionPlan> region(int rx, int rz) const;
    static int regionOf(int coordinate);
    // CIUDADES-GRANDES: la metropolis de la celda (mi, mj) de 3 x 3 regiones (nullptr si no
    // hay). La celda 0,0 (regiones -1..1, la capital) nunca tiene.
    std::shared_ptr<const Structure> metropolis(int mi, int mj) const;
    static int megaOf(int region);
    // CIUDADES-GRANDES: cofres de botin generados (puntos "cofre_botin") en la caja (x, z
    // absolutos, incluidos). Ver GeneratedChest.
    std::vector<GeneratedChest> chests(int x0, int z0, int x1, int z1) const;

private:
    std::shared_ptr<const RegionPlan> planRegion(int rx, int rz) const;
    std::shared_ptr<const Structure> planMetropolis(int mi, int mj) const;

    const Generator& gen_;
    std::string dimension_;
    mutable std::mutex mutex_;
    // Plan de cada region (o el que se esta haciendo: los demas hilos esperan en vez de repetirlo).
    mutable std::unordered_map<std::int64_t, std::shared_future<std::shared_ptr<const RegionPlan>>> cache_;
    mutable std::list<std::int64_t> order_;  // para olvidar las mas viejas
    mutable std::mutex metro_mutex_;
    mutable std::unordered_map<std::int64_t, std::shared_future<std::shared_ptr<const Structure>>> metro_cache_;
    mutable std::list<std::int64_t> metro_order_;
};

// --- Construccion (StructureBuild.cpp) ------------------------------------------------

// Estilo de los edificios segun el bioma.
// Elven: aldea del bosque (madera clara, tejas verdes); Swamp: casas sobre pilotes.
enum class BuildStyle : int { Temperate = 0, Snow, Desert, Beach, City, Elven, Swamp };

// Lo que necesitan los constructores: el plano, los puntos y un azar propio.
struct BuildContext {
    Blueprint bp;
    std::vector<LocalPoint> points;
    std::uint32_t seed = 0;
    BuildStyle style = BuildStyle::Temperate;
    BlockId roof = block::RoofRed;
    float rand(int a, int b = 0, int c = 0) const;  // 0..1 determinista
    int randInt(int lo, int hi, int a, int b = 0) const;  // [lo, hi]
    void point(const std::string& kind, float x, float y, float z, int dir, const std::string& tag = {}) {
        points.push_back(LocalPoint{kind, x + 0.5f, y, z + 0.5f, dir, tag});
    }
};

// Constructores: cada uno llena ctx.bp (que crean ellos) y ctx.points. La
// fachada (puerta) mira a -z. `floors` pisos de 4 de alto.
namespace build {
// Casas: w x d de planta (sin aleros), 1..3 pisos. Ocupa (w + 2) x (d + 2).
void house(BuildContext& ctx, int w, int d, int floors, const std::string& npc_tag);
void tavern(BuildContext& ctx);
void smithy(BuildContext& ctx, bool city);
void shop(BuildContext& ctx, const std::string& kind);  // "pociones", "armaduras", "general"
void stable(BuildContext& ctx);
void temple(BuildContext& ctx);
void well(BuildContext& ctx);
void fountain(BuildContext& ctx, int size);
void stall(BuildContext& ctx, BlockId cloth, const std::string& shop);
void farm(BuildContext& ctx, int w, int d);
void lampPost(BuildContext& ctx);
void tree(BuildContext& ctx);
void bench(BuildContext& ctx);
// Ciudad.
void wallSegment(BuildContext& ctx, int length, int height);
void tower(BuildContext& ctx, int size, int height);
void gatehouse(BuildContext& ctx, int width, int height, int wall_thickness);
void castle(BuildContext& ctx, int w, int d, bool vault = false);  // CIUDADES-GRANDES: vault = camara sellada bajo el patio
void pier(BuildContext& ctx, int length, int sea);
// Sueltas.
void wizardTower(BuildContext& ctx, int floors);
void ruin(BuildContext& ctx, int size);
void portal(BuildContext& ctx, const std::string& dimension);
// Mazmorras (coordenadas locales; la sala se construye con su cascara).
enum class RoomKind : int { Hall = 0, Library, Crypt, Prison, Lava, Flooded, Trap, Treasure, Boss, Plain };
void dungeonRoom(BuildContext& ctx, int iw, int ih, int id, RoomKind kind, int level, const std::string& spawn,
                 const std::string& boss, int shaft);  // 0 sin escalera, 1 llega desde arriba, 2 baja desde aqui
void corridor(BuildContext& ctx, int length, bool trap);
// Escalera de caracol de `depth` de bajada (pozo de 5 x 5): arriba abierta
// (con barandilla) y abajo con salida.
void spiralShaft(BuildContext& ctx, int depth, BlockId wall);
void dungeonEntrance(BuildContext& ctx, int shaft_depth);

// --- Asentamientos nuevos (StructureSettlements.cpp) ---
// Farola de calle (1 x 5 x 1, como lampPost) con los bloques de luz si existen.
void streetLamp(BuildContext& ctx);
// Antorchas altas cada `every` en el adarve de un plano de wallSegment ya hecho.
void wallTorches(BuildContext& ctx, int every);
// Estanque con juncos (pantano). El plano empieza en el suelo (y 0 = agua).
void pond(BuildContext& ctx, int w, int d);
// CIUDADES-AMPLIAS: jardin entre casas (seto, arbol, flores, bancos); la entrada mira a -z.
void garden(BuildContext& ctx, int w, int d);
// Arbol mayor de la plaza de las aldeas elficas (15 x 15), con farolillos y bancos.
void greatTree(BuildContext& ctx);
// Sube el plano ya construido `dy` sobre pilotes y le pone escalera delante
// (el plano crece dy en alto y dy en fondo, por delante).
void raise(BuildContext& ctx, int dy);
// Plazas hostiles (un solo plano; la entrada mira a -z). Dejan puntos
// "spawner" (tag = especie), "jefe" (tag = especie) y "cofre" (tag = calidad).
void banditFort(BuildContext& ctx, int size, const std::string& grunt, const std::string& archer, const std::string& boss);
void orcCamp(BuildContext& ctx, int radius, const std::string& grunt, const std::string& archer, const std::string& boss);
void undeadCastle(BuildContext& ctx, int w, int d, const std::vector<std::string>& garrison, const std::string& boss);
void cursedCity(BuildContext& ctx, int size, const std::vector<std::string>& garrison, const std::string& boss);
void cultistTower(BuildContext& ctx, int floors, const std::string& minion, const std::string& boss);

// --- CIUDADES-GRANDES (StructureCity.cpp): metropolis, castillos y misterios ---
// Cofres de botin: bloque Chest + punto "cofre_botin" (tag "<nivel 1..4>:<tabla>"). Notas:
// bloque LoreTablet + punto "nota" (tag = tema). Puzles: puntos "puzle" / "puzle_puerta".
void lootChest(BuildContext& ctx, int x, int y, int z, int dir, int tier, const std::string& table);
void loreNote(BuildContext& ctx, int x, int y, int z, int dir, const std::string& topic);
// Catedral (W x D ~ 23 x 41) con dos campanarios, naves, cripta debajo (el plano empieza
// kCryptDepth bajo el suelo) y un osario sellado tras un muro falso con un cofre.
inline constexpr int kCryptDepth = 7;
void cathedral(BuildContext& ctx);
// Sede de gremio (15 x 13, 3 pisos); `guild` = "mercaderes" | "herreros" | "magos" | "ladrones"...
// Detras de una estanteria falsa, una camara con cofre.
void guildHall(BuildContext& ctx, const std::string& guild);
// Casona noble con jardin cerrado (21 x 27): fuente, setos y estudio secreto.
void manor(BuildContext& ctx);
// Taller ("curtidor" | "carpintero" | "tejedor" | "alfarero"): planta abierta con su oficio.
void workshop(BuildContext& ctx, const std::string& kind);
// Cuartel (17 x 11, 2 pisos) y patio de armas (w x d, vallado, munecos y dianas).
void barracks(BuildContext& ctx);
void trainingYard(BuildContext& ctx, int w, int d);
// Almacen del puerto (13 x 11).
void warehouse(BuildContext& ctx);
// Banco (13 x 12) con camara acorazada debajo (el plano empieza 6 bajo el suelo).
void bank(BuildContext& ctx);
// Huerto de frutales (w x d) y molino de viento (9 x 9).
void orchard(BuildContext& ctx, int w, int d);
void windmill(BuildContext& ctx);
// Cementerio (w x d) con mausoleo y cripta debajo (el plano empieza 6 bajo el suelo).
void graveyard(BuildContext& ctx, int w, int d);
// Pozo seco con escalera de caracol hasta los tuneles (depth bajo el suelo; el plano empieza ahi).
void dryWell(BuildContext& ctx, int depth);
// Sala subterranea (iw x id por dentro, 4 de alto): "guarida" | "boveda" | "puzle" | "cripta".
void undergroundRoom(BuildContext& ctx, int iw, int id, const std::string& kind, int tier);
// Tunel subterraneo (3 de ancho) de `length` hacia +z.
void tunnel(BuildContext& ctx, int length);
// Santuario misterioso fuera de las ciudades (variant 0 circulo de piedras, 1 capilla en ruinas,
// 2 obelisco de runas); cripta sellada debajo (el plano empieza 6 bajo el suelo).
void shrine(BuildContext& ctx, int variant);
// Casa adosada de ciudad con mas variedad (voladizo en los pisos de arriba, tejado a cuatro
// aguas o buhardillas, a veces bodega con cofre). Ocupa (w + 2) x (d + 2).
void townhouse(BuildContext& ctx, int w, int d, int floors, const std::string& npc_tag, int variant, bool cellar);
}  // namespace build

}  // namespace cramion::voxel

#endif  // CRAMION_CORE_VOXEL_STRUCTURES_H
