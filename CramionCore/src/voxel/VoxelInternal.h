#ifndef CRAMION_CORE_VOXEL_INTERNAL_H
#define CRAMION_CORE_VOXEL_INTERNAL_H

// Piezas internas del mundo de bloques: la columna (chunk), el generador y
// el mallador. VoxelSystem.cpp las une.

#include "CramionCore/voxel/Voxel.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cramion::voxel {

// Indice dentro de una columna: x + z * 16 + y * 256.
inline int cellIndex(int x, int y, int z) { return x + z * kChunkSize + y * kChunkSize * kChunkSize; }

inline constexpr int kChunkCells = kChunkSize * kChunkSize * kWorldHeight;

// IDS16: los bloques son de 16 bits (2 bytes por celda). Para no gastar el doble de memoria, al terminar de generar
// (VoxelSystem, hilo de fondo) se recorta el aire de arriba: `blocks` llega solo hasta la ultima seccion (16 de alto)
// con algun bloque; lo de encima es aire sin memoria. Generar y leer el guardado trabajan con la columna entera
// (recien creada, sin recortar). Despues, leer con get() / at() y escribir con set() (crece si hace falta).
struct ChunkData {
    int cx = 0;
    int cz = 0;
    std::vector<BlockId> blocks = std::vector<BlockId>(kChunkCells, block::Air);
    // Luz: cielo en los 4 bits altos, bloques en los bajos.
    std::vector<std::uint8_t> light = std::vector<std::uint8_t>(kChunkCells, 0);
    // Tinte de hierba y hojas por columna (RGB 4:4:4) segun el bioma.
    std::array<std::uint16_t, kChunkSize * kChunkSize> grass_tint{};
    std::array<std::uint16_t, kChunkSize * kChunkSize> foliage_tint{};
    bool modified = false;  // cambiado por el juego: se guarda

    BlockId get(int x, int y, int z) const { return at(static_cast<std::size_t>(cellIndex(x, y, z))); }
    // Por indice (cellIndex): aire por encima de lo recortado.
    BlockId at(std::size_t i) const { return i < blocks.size() ? blocks[i] : block::Air; }
    void set(std::size_t i, BlockId id) {
        if (i >= blocks.size()) {
            if (id == block::Air) return;
            constexpr std::size_t kSection = static_cast<std::size_t>(kChunkSize) * kChunkSize * kChunkSize;
            blocks.resize((i / kSection + 1) * kSection, block::Air);
        }
        blocks[i] = id;
    }
    // Quita el aire de arriba (secciones enteras).
    void trimAir() {
        constexpr std::size_t kSection = static_cast<std::size_t>(kChunkSize) * kChunkSize * kChunkSize;
        std::size_t top = blocks.size();
        while (top > 0 && blocks[top - 1] == block::Air) --top;
        const std::size_t keep = (top + kSection - 1) / kSection * kSection;
        if (keep < blocks.size()) {
            blocks.resize(keep);
            blocks.shrink_to_fit();
        }
    }
};

// --- Generador ---------------------------------------------------------------------

class Structures;  // Structures.h

class Generator {
public:
    Generator(int seed, int sea_level, VoxelGenerator profile = VoxelGenerator::Classic);
    // Rellena la columna (bloques y tintes).
    void generate(ChunkData& chunk) const;
    // Altura del terreno (sin cuevas ni arboles): el ultimo bloque solido.
    int terrainHeight(int x, int z) const;
    // El primer bloque por encima del agua de la columna (el mar: sea_; los
    // lagos de montana, mas arriba), o 0 si no hay agua sobre el terreno.
    int waterLevel(int x, int z) const;
    int seaLevel() const { return sea_; }
    int seed() const { return seed_; }
    // Arboles (VoxelWorld::tree_density y clearings). Con 1 y 0 (lo de
    // siempre) el mundo sale identico. Llamar antes de generar (no es seguro
    // cambiarlo mientras otros hilos generan).
    void setVegetation(float tree_density, float clearings);
    float treeDensity() const { return tree_density_; }
    float clearings() const { return clearings_; }
    // Probabilidad de un arbol en (x, z) tras aplicar la densidad y los claros
    // (`chance` = la del bioma). Igual a `chance` con densidad 1 y sin claros.
    float treeChance(float chance, int x, int z) const;
    // Entradas de cueva del generador Aventura (VoxelWorld::cave_entrances):
    // 0 = solo las aberturas naturales de antes (el mundo de siempre), 1 = bocas
    // en laderas, simas con rampa y alguna grieta (~1 cada 160 m, mas en las
    // montanas), 2 = el doble. Llamar antes de generar (como setVegetation).
    void setCaveEntrances(int level);
    int caveEntrances() const { return cave_entrances_; }
    // Cuevas inmensas lejos del pueblo de inicio (GeneratorCaves.cpp; por defecto si).
    // Sin ellas, el mundo de antes bloque a bloque. Llamar antes de generar.
    void setImmenseCaves(bool on) { immense_caves_ = on; }
    bool immenseCaves() const { return immense_caves_; }
    // Oceanos hondos (GeneratorAdventure.cpp; por defecto si): plataforma, talud, llanura
    // abisal (y ~12-18), dorsales, montes submarinos, fosas, canones, kelp, corales y
    // oceanos mas grandes lejos del origen. Nada cambia a menos de ~420 m del origen.
    // Sin ellos, el mundo de antes bloque a bloque. Llamar antes de generar.
    void setDeepOceans(bool on) { deep_oceans_ = on; }
    bool deepOceans() const { return deep_oceans_; }
    // Mena de arcanita (senales, Rails.h; por defecto si): en las vetas hondas (y < 40) en lugar
    // de parte del carbon. Sin ella, el mundo de antes bloque a bloque. Llamar antes de generar.
    void setArcaniteOre(bool on) { arcanite_ore_ = on; }
    // NOCHE-MUNDO: biomas frondosos de las tierras salvajes (GeneratorLush.cpp; por defecto si): selva,
    // cerezos, pantano, sabana y bosque de otono en las provincias que eran tierra normal (lejos del
    // origen). Sin ellos, el mundo de antes bloque a bloque. Llamar antes de generar.
    void setLushBiomes(bool on) { lush_biomes_ = on; }
    bool lushBiomes() const { return lush_biomes_; }
    // NATURALEZA-HZ (GeneratorNature.cpp): arboles con mas especies (sauce, acacia, arbol gigante),
    // cactus con formas (saguaro, grupos, chumbera), afloramientos de mineral y cristal que asoman,
    // rocas, troncos caidos, arbustos y cordilleras masivas con valles y mesetas. Para no descolocar
    // las ediciones guardadas por coordenada (world_edits del servidor), nada de esto cambia dentro
    // de las kNaturaRing regiones (Structures.h kRegionSize, Chebyshev) mas cercanas al origen; el
    // relieve entra con una rampa de kNaturaBlend m (sin escalon). Ver natureWeight / natureNew.
    static constexpr int kNaturaRing = 2;        // como CIUDADES-AMPLIAS kAmpleRing (0 = en todo el mundo)
    static constexpr int kNaturaMargin = 16;     // m de mas: las copas de un arbol nuevo no entran en lo viejo
    static constexpr float kNaturaBlend = 256.0f;  // rampa del relieve nuevo (m)
    static constexpr bool kNaturaDefault = true;   // por defecto (setNature para las pruebas)
    void setNature(bool on) { nature_ = on; }
    bool nature() const { return nature_; }
    // Borde (Chebyshev, m) a partir del que hay naturaleza nueva.
    static constexpr int natureEdge() { return kNaturaRing <= 0 ? 0 : kNaturaRing * 384 - 384 / 2 + kNaturaMargin; }
    // 0 = el mundo de antes .. 1 = relieve nuevo del todo (rampa suave desde el borde).
    float natureWeight(float x, float z) const;
    // Arboles, cactus y afloramientos nuevos en (x, z) (su pie): todo o nada.
    bool natureNew(int x, int z) const;
    static bool isLushRegion(int region) { return region >= WJungle && region <= WAutumn; }
    static bool isLushBiome(int biome) { return biome >= AJungle && biome <= AAutumn; }
    // Agua del mar sobre el suelo de (x, z) en bloques (0 = tierra, lago o sin mar).
    // Barato (sin generar el chunk): para buscar donde soltar criaturas marinas.
    int oceanDepth(int x, int z) const;
    // Una entrada (pruebas y depuracion): donde se entra desde la superficie
    // (start: el primer bloque de aire sobre el suelo) y donde acaba el tunel.
    struct CaveEntranceInfo {
        int kind = 0;  // 0 boca en una ladera, 1 sima con rampa, 2 grieta
        int x = 0, y = 0, z = 0;
        int end_x = 0, end_y = 0, end_z = 0;
        bool connected = false;  // el tunel llega a una cueva natural
        int min_x = 0, min_z = 0, max_x = 0, max_z = 0;  // lo que excava
    };
    // Las entradas que empiezan en el rectangulo [x0, x1) x [z0, z1).
    std::vector<CaveEntranceInfo> caveEntrancesIn(int x0, int z0, int x1, int z1) const;
    // Nivel de los mares de lava del abismo.
    static constexpr int kAbyssLava = 34;
    VoxelGenerator profile() const { return profile_; }
    // Aldeas, ciudades, mazmorras... (Structures.h); nulo si el generador no tiene.
    const Structures* structures() const { return structures_.get(); }

    // Biomas del generador Aventura (pruebas y depuracion).
    // Los cuatro ultimos (tierras salvajes, GeneratorWild.cpp) solo salen lejos
    // del origen: el mundo de alrededor del origen es el de siempre.
    enum AdventureBiome : int {
        AOcean = 0, ABeach, ARiver, AMeadow, AForest, ADesert, ASnow, AMountains,
        ABadlands,    // Tierras Carmesi: arena roja, arenisca roja, mesetas por escalones y chimeneas de hadas
        ARoseDesert,  // Desierto Rosa: dunas de arena rosa y agujas de cuarzo rosa
        AAshlands,    // Paramo de Ceniza: basalto, ceniza, volcanes y charcas de lava
        ABlight,      // Tierras Marchitas: hierba marchita, espinas, arboles muertos y puas de obsidiana
        // NOCHE-MUNDO: biomas frondosos (GeneratorLush.cpp), en las provincias que antes eran tierra normal.
        AJungle,      // Verdant Jungle: colinas, arboles de 25-40 m con lianas, helechos
        ABlossom,     // Blossom Vale: lomas suaves, cerezos en flor, petalos y estanques
        ASwamp,       // Mirefen Swamp: llano bajo con charcas, barro, manglares y nenufares
        ASavanna,     // Sunscorch Savanna: llanura seca, acacias, hierba alta y termiteros
        AAutumn,      // Emberleaf Woods: bosque de otono (naranja, rojo, amarillo) y hojarasca
        kAdventureBiomeCount
    };
    // Provincias de las tierras salvajes (celdas de ~1,6 km lejos del origen).
    enum WildRegion : int { WNone = 0, WPeaks, WSky, WBadlands, WRose, WAsh, WBlight,
                            WJungle, WBlossom, WSwamp, WSavanna, WAutumn };  // NOCHE-MUNDO: los frondosos
    struct AdventureColumn {
        int height = 64;
        float temperature = 0.0f;  // -1 frio .. 1 caluroso
        float humidity = 0.0f;     // -1 seco .. 1 humedo
        float river = 0.0f;        // 0 lejos .. 1 en el cauce
        bool mesa = false;         // meseta del desierto (terracota)
        int biome = AMeadow;
        int water = 0;             // primer bloque por encima del agua (mar o lago); <= height: seca
        bool lake = false;         // en un lago (o su orilla)
        // Tierras salvajes (0 / falso / WNone cerca del origen).
        int region = WNone;        // provincia (WildRegion)
        float strength = 0.0f;     // 0 borde .. 1 dentro de la provincia
        bool giant = false;        // cordillera gigante (picos de mas de 150 m sobre el mar)
        bool peak = false;         // NATURALEZA-HZ: cumbre de las cordilleras nuevas (como giant en los bloques)
        int lava = 0;              // primer bloque por encima de la lava (charca, crater); <= height: sin lava
        int feature = 0;           // relieve suelto: 1 chimenea de hadas, 2 pua, 3 cono volcanico
        int feature_base = 0;      // altura del suelo sin ese relieve
        // Oceanos hondos (lejos del pueblo de inicio; 0 cerca: el fondo de siempre).
        float ocean = 0.0f;        // 0 el fondo de siempre .. 1 el fondo hondo nuevo
        int seabed = 0;            // roca del fondo: 0 sedimento, 1 basalto (dorsal, monte), 2 fosa
        // Rios nuevos (lejos del pueblo; 0 cerca: el rio de siempre, al nivel del mar): el
        // primer bloque por encima del agua del cauce (baja hacia el mar por escalones:
        // cascadas donde cambia) y un resalte de mas de 4 bloques justo debajo (poza).
        int river_level = 0;
        // NOCHE-MUNDO: charcas del pantano y estanques del valle de los cerezos: el primer bloque por
        // encima de su agua (<= height: sin charca). adventureColumn la pasa a `water`.
        int pond = 0;
    };
    AdventureColumn adventureColumn(int x, int z) const;

    // Terreno lejano (FarTerrain.cpp, estilo "Distant Horizons"): lo que se ve
    // de una columna desde lejos, sin generar el chunk (GeneratorFar.cpp).
    struct FarColumn {
        int height = 64;              // ultimo bloque solido del suelo
        BlockId top = block::Grass;   // bloque de arriba
        BlockId under = block::Dirt;  // el de debajo (taludes bajos)
        BlockId deep = block::Stone;  // la roca (acantilados)
        int water = 0;                // primer bloque por encima del agua (0 = seca)
        bool sea = false;             // ese agua es el mar (nivel del mar)
        int canopy = 0;               // copa de los arboles: ultimo bloque de hojas (0 = sin arbol)
        BlockId leaves = block::OakLeaves;
        bool cliffable = false;       // con pendiente fuerte el bloque de arriba pasa a ser `deep`
        std::uint16_t grass_tint = 0xFFF;
        std::uint16_t foliage_tint = 0xFFF;
        // Isla flotante encima (0 = ninguna): primer y ultimo bloque.
        int island_bottom = 0;
        int island_top = 0;
        BlockId island_block = block::Grass;  // el de arriba
        BlockId island_rock = block::SkyStone;  // la panza
    };
    // false si el generador no tiene terreno lejano (dimensiones). `detailed`:
    // tambien el suelo aplanado de aldeas y ciudades y sus claros (mas caro;
    // solo para lo que se ve a menos de ~1 km).
    bool farColumn(int x, int z, FarColumn& out, bool detailed = true) const;

    // --- Reino Profundo (GeneratorDeep.cpp) ---
    // Simas abisales de la superficie de Aventura (lejos del origen, una como mucho por
    // celda de kSinkCell m): un pozo redondo desde el suelo hasta un estanque en y 4..10
    // por el que se baja al Reino Profundo ("profundo"). En el Reino Profundo, bajo cada
    // sima hay una "caida": un pozo desde el techo hasta un lago, y la Piedra de Ascenso.
    static constexpr int kSinkCell = 640;
    struct Sinkhole {
        bool valid = false;
        int x = 0, z = 0;  // centro
        float r = 7.0f;    // radio del pozo
        int top = 64;      // suelo de la superficie en el centro
        float phase = 0.0f;
        std::uint32_t id = 0;
    };
    Sinkhole sinkhole(int gx, int gz) const;
    // Estructuras del Reino Profundo (y las simas en la superficie) que tocan la esfera,
    // o la que contiene el punto (coordenadas absolutas). Vacio si el generador no tiene.
    std::vector<StructureInfo> deepStructuresNear(float x, float y, float z, float radius) const;
    bool deepStructureAt(float x, float y, float z, StructureInfo& out) const;
    struct DeepSite;
    // Lo que hay en una region de kDeepRegion m del Reino Profundo (cacheado).
    std::vector<std::shared_ptr<const DeepSite>> deepRegion(int rx, int rz) const;
    // Columna del Reino Profundo (pruebas): suelo y techo de las dos capas.
    struct DeepColumn {
        int lf = 30;    // suelo de abajo (ultimo bloque solido)
        int lc = 120;   // techo de abajo (primer bloque solido de la corteza)
        int uf = 150;   // suelo de arriba (ultimo bloque de la corteza)
        int uc = 236;   // techo de arriba (primer bloque del techo)
        int lower = 0;  // bioma de abajo: 0 salas de goteo, 1 hongos, 2 picos de cristal, 3 brasas
        int upper = 0;  // bioma de arriba: 0 bosque de estalactitas, 1 selva de hongos, 2 gruta de cristal
        bool chasm = false;   // la corteza se abre (sima entre las capas)
        float river = 0.0f;   // rio de lava (brasas)
        bool well = false;    // pozo de la caida (bajo una sima): abierto del techo al lago
        float well_d = 1e9f;  // distancia al centro de la caida
        bool hole = false;    // agujero del suelo (pozo a la capa de abajo): abierto hasta el estanque del fondo
        float hole_d = 1e9f;  // distancia al centro del agujero
    };
    // `hole`: el agujero del suelo hacia la capa de abajo (capas 1..7; nulo: ninguno).
    DeepColumn deepColumn(int x, int z, const Sinkhole* fall, const Sinkhole* hole = nullptr) const;
    static constexpr int kDeepSea = 40;       // primer bloque sin agua del mar sin sol (capa de abajo)
    static constexpr int kDeepRegion = 256;
    static constexpr int kDeepArrival = 246;  // y de llegada (arriba del pozo de la caida)

    // --- Capas del Reino Profundo (GeneratorDeep.cpp) ---
    // Ocho capas apiladas de 256 bloques: "profundo" (1, The Sunless Deep), "profundo2".."profundo8".
    // Entre la capa k y la k+1 (frontera k), pozos en una rejilla de kShaftCell m: en la capa k un
    // agujero en el suelo (estanque en y 2..9: el juego baja de capa al caer en el) y en la k+1, en el
    // mismo x, z, el pozo del techo al lago con la Escalera de los Delvers (subida) y la Piedra de Subida.
    static constexpr int kDeepLayers = 8;
    static constexpr int kShaftCell = 384;
    static constexpr int kShaftPool = 10;  // y del punto "portal" del estanque del agujero
    // Capa (1..8) de un generador Deep (la pone VoxelSystem segun la dimension). Llamar antes de generar.
    void setDeepLayer(int layer);
    int deepLayer() const { return deep_layer_; }
    // Pozo de la frontera `boundary` (1..7) en la celda (gx, gz) y el de la celda que contiene (x, z).
    // `top`: suelo de la plataforma del agujero (solo calculado si boundary == deepLayer()).
    Sinkhole layerShaft(int boundary, int gx, int gz) const;
    Sinkhole layerShaftAt(int boundary, int x, int z) const;

private:
    int deep_layer_ = 1;
    Sinkhole shaftCandidate(int boundary, int gx, int gz) const;
    std::shared_ptr<const DeepSite> planShaftHole(const Sinkhole& s) const;
    struct DeepCache;
    std::shared_ptr<DeepCache> deep_cache_;
    static std::shared_ptr<DeepCache> makeDeepCache();
    std::shared_ptr<Generator> surface_;  // Reino Profundo: la superficie de Aventura (donde estan las simas)
    const Generator& surfaceGenerator() const { return surface_ ? *surface_ : *this; }
    void generateDeep(ChunkData& chunk) const;
    // Superficie de Aventura: excava las simas abisales del chunk (`heights`: el suelo de cada columna).
    void carveSinkholes(ChunkData& chunk, const std::array<int, kChunkSize * kChunkSize>& heights) const;
    std::shared_ptr<const DeepSite> planDeepSite(int rx, int rz) const;
    std::shared_ptr<const DeepSite> planAscension(const Sinkhole& s) const;
    void buildDeepSite(const DeepSite& site, ChunkData* chunk, std::vector<StructurePoint>* points) const;

    struct Column {
        int height = 64;
        float temperature = 0.0f;  // -1 frio .. 1 caluroso
        float humidity = 0.0f;     // -1 seco .. 1 humedo
        int biome = 0;
    };
    Column column(int x, int z) const;
    float noise2(float x, float z, int octave_seed) const;
    float fbm2(float x, float z, int octaves, int salt) const;
    float noise3(float x, float y, float z, int salt) const;
    void placeTrees(ChunkData& chunk) const;
    // Margen sin arboles alrededor de aldeas y caminos (0 con los valores de siempre).
    float treeClearMargin() const;
    // Generador Aventura (GeneratorAdventure.cpp).
    // Lagos a cualquier altura: uno como mucho por celda de 160 m, donde el
    // terreno es llano; su nivel, el borde mas bajo (se calcula una vez).
    struct Lake {
        bool valid = false;
        float x = 0.0f, z = 0.0f, radius = 0.0f, depth = 0.0f;
        int level = 0;  // y del agua de arriba
    };
    struct LakeCache;
    Lake adventureLake(int gx, int gz) const;
    AdventureColumn adventureColumnBase(int x, int z) const;
    std::shared_ptr<LakeCache> lakes_;
    void generateAdventure(ChunkData& chunk) const;
    // Oceanos hondos (GeneratorAdventure.cpp): cuanto se hunde el continente lejos del
    // origen (oceanos grandes), cuanto pesa el fondo nuevo por la distancia al origen,
    // el fondo en (x, z) (`e` = -continente > 0) y el bloque del fondo.
    float oceanBasin(float x, float z) const;
    float oceanWeight(float x, float z) const;
    float oceanFloor(float x, float z, float e, float hills, int& seabed) const;
    BlockId seabedBlock(const AdventureColumn& c, int x, int z, int depth) const;
    // OCEANOS-HZ (GeneratorOcean.cpp): continentes y oceanos de varios km, hondos, con plataforma,
    // archipielagos, costas de playa o acantilado y rios mas escasos y estrechos. 0 dentro de las
    // kOceanRing regiones del origen (Chebyshev; una mas que NATURALEZA-HZ: las aldeas de la segunda
    // corona despejan arboles hasta 16 m dentro de la primera, asi que su plan no puede cambiar)
    // mas kOceanMargin m, y rampa de kOceanBlend m.
    static constexpr int kOceanRing = 3;
    static constexpr int kOceanMargin = 32;
    static constexpr float kOceanBlend = 512.0f;
    static constexpr int oceanEdge() { return kOceanRing <= 0 ? 0 : kOceanRing * 384 - 384 / 2 + kOceanMargin; }
    float oceanScaleWeight(float x, float z) const;
    float oceanContinent(float x, float z) const;  // el campo continente nuevo (< 0 mar)
    float oceanRiverNoise(float x, float z) const;  // ruido de los rios nuevo (con signo; 0 en el cauce)
    float oceanCoast(float x, float z, float continent, float h) const;  // playas y acantilados
    float oceanShelf(float x, float z) const;  // ancho de la plataforma (0,25 = cuatro veces mas ancha .. 1)
    float oceanFbm(float x, float z, int octaves, int k) const;  // fbm2 sin perder precision (desfase < 256)
    void placeAdventureTrees(ChunkData& chunk) const;
    // NATURALEZA-HZ (GeneratorNature.cpp): cordilleras, valles y mesetas (sobre la altura `h` de la
    // columna; devuelve la nueva) y lo que se pone sobre el suelo antes de los arboles.
    float natureRelief(float x, float z, float land, float inland, float river_noise, float h, AdventureColumn& c) const;
    void placeNatureDecor(ChunkData& chunk) const;
    // NOCHE-MUNDO (GeneratorLush.cpp): relieve, bloques, plantas y arboles de los biomas frondosos.
    void lushShape(float x, float z, float land, int stage, float k, float& h, AdventureColumn& c) const;
    BlockId lushBlock(const AdventureColumn& c, int x, int y, int z, int depth, int soil, bool cliff, bool underwater) const;
    BlockId lushPlant(const AdventureColumn& c, BlockId ground, int x, int z, float r) const;
    void lushTint(const AdventureColumn& c, std::uint16_t& grass, std::uint16_t& foliage) const;
    void placeLushTrees(ChunkData& chunk) const;
public:
    // Terreno lejano de los biomas frondosos (GeneratorFar.cpp): bloques de la columna y la copa.
    void lushFar(const AdventureColumn& c, int x, int z, FarColumn& out) const;
private:
    // Cuevas del Aventura (GeneratorCaves.cpp): la densidad en un nudo de la
    // rejilla de 4 m y la interpolada en un bloque (> 0: hueco).
    float caveDensity(float wx, float wy, float wz) const;
    float caveField(int x, int y, int z) const;
    // Cuevas inmensas (GeneratorCaves.cpp), lejos del pueblo de inicio: 0 cerca del
    // origen (las cuevas de siempre) .. 1 a partir de ~650 m.
    float caveDeep(float wx, float wz) const;
    float immenseDensity(float wx, float wy, float wz, float deep, float a, float b) const;
    // Zona de lagos (> 0) o de lava (< 0) de las cuevas y el anillo de roca que la cierra.
    float liquidZone(float wx, float wz, float deep) const;
    float liquidRing(float wx, float wy, float wz, float deep) const;
    // Lo mas bajo a lo que puede bajar un tunel de entrada (0 = sin limite).
    int caveFloorLimit(float wx, float wz) const;
    enum CaveBiome : int { kCaveNormal = 0, kCaveMushroom, kCaveCrystal, kCaveDripstone, kCaveFrozen };
    // Lo de una columna del chunk para las cuevas inmensas.
    struct CaveColumn {
        float deep = 0.0f;           // 0: las cuevas de siempre (nada de lo de abajo)
        int surface = 0;             // suelo de la columna
        bool dry = false;            // ni agua ni lava encima
        int liquid_top = 0;          // el hueco de cueva hasta aqui es `liquid`
        BlockId liquid = block::Air;
        int deepstone = 0;           // roca honda por debajo
        int biome = kCaveNormal;     // CaveBiome
        int open_lo = kWorldHeight;  // primer y que puede abrir un barranco o un pozo
        bool rav = false, shaft = false;
        float rav_d = 0.0f, rav_w = 0.0f, phase = 0.0f;
        int rav_floor = 0;
        float shaft_d = 0.0f, shaft_r = 0.0f;
        int shaft_floor = 0;
    };
    // `cols`: las columnas del chunk con un borde de 1 ((x + 1) + (z + 1) * stride).
    void caveColumns(int x0, int z0, const AdventureColumn* cols, int stride, std::array<CaveColumn, kChunkSize * kChunkSize>& out) const;
    // El barranco o el pozo de la columna abre y (hasta el cielo).
    static bool caveOpen(const CaveColumn& c, int y);
    // (x, z) en un barranco o un pozo, o junto a el (sin arboles ahi).
    bool caveOpenNear(int x, int z) const;
    float ravineLine(float wx, float wz) const;
    bool ravineAt(float wx, float wz, float deep, float& dist, float& width, float& strength) const;
    struct CaveShaft;
    void caveShafts(int x0, int z0, int x1, int z1, std::vector<CaveShaft>& out) const;
    struct MineSegment;
    void mineSegments(int x0, int z0, int x1, int z1, std::vector<MineSegment>& out) const;
    // Grutas (setas, cristal, espeleotemas, hielo), orillas de lagos y lava y minas.
    void decorateCaves(ChunkData& chunk, const std::array<CaveColumn, kChunkSize * kChunkSize>& caves) const;
    // Entradas de cueva: una como mucho por celda de 64 x 64 m (se calcula una vez).
    struct CaveEntrance;
    struct EntranceCache;
    std::shared_ptr<const CaveEntrance> caveEntrance(int gx, int gz) const;
    // Lo que las entradas excavan en las columnas de un chunk.
    struct EntranceCarve {
        bool any = false;
        std::array<int, kChunkSize * kChunkSize> lo{};  // primer bloque de aire (>= hi+1: nada)
        std::array<int, kChunkSize * kChunkSize> hi{};  // ultimo bloque de aire
        std::array<std::uint8_t, kChunkSize * kChunkSize> flags{};  // 1 suelo macizo bajo lo, 2 luz
    };
    // `heights`: la altura de las columnas del chunk (x + z * 16).
    void caveEntranceCarve(int x0, int z0, const std::array<int, kChunkSize * kChunkSize>& heights, EntranceCarve& out) const;
    // Las entradas cuya zona toca el rectangulo [x0, x1] x [z0, z1].
    std::vector<std::shared_ptr<const CaveEntrance>> entrancesTouching(int x0, int z0, int x1, int z1) const;
    // (x, z) a menos de `margin` de lo que excava alguna de `list` (sin arboles ahi).
    static bool nearCaveEntrance(const std::vector<std::shared_ptr<const CaveEntrance>>& list, int x, int z, float margin);
public:
    // Tierras salvajes (GeneratorWild.cpp): lejos del origen, provincias de
    // cordilleras gigantes, archipielagos flotantes y biomas peligrosos.
    struct WildCell {
        int region = WNone;
        float strength = 0.0f;  // 0 en el borde .. 1 dentro
        float cx = 0.0f, cz = 0.0f;
    };
    WildCell wildRegion(float x, float z) const;
    // Da forma al relieve de las tierras salvajes. `stage` 0: antes del clima
    // (cordilleras); 1: despues del clima (biomas peligrosos); 2: despues de
    // los rios (lava).
    void wildShape(float x, float z, float land, float inland, int stage, float& h, AdventureColumn& c) const;
    int wildBiome(const AdventureColumn& c) const;
    // El bloque de los biomas salvajes a `depth` bajo la superficie; Air = la
    // roca normal (con sus menas).
    BlockId wildBlock(const AdventureColumn& c, int x, int y, int z, int depth, int soil, bool cliff, bool underwater) const;
    // Islas flotantes del mundo normal (GeneratorWild.cpp): pocas y grandes
    // (80-250 m), con acantilados, panza de montana invertida y cuevas.
    struct WildIsland {
        float cx = 0.0f, cz = 0.0f, r = 60.0f;  // centro y radio medio (la costa es irregular)
        float top = 190.0f;                     // cota base del cesped
        float slab = 16.0f, depth = 40.0f;      // grosor del borde y de la panza (montana invertida)
        std::uint32_t id = 0;
        std::uint8_t kind = 0;                  // 0 bosque, 1 pradera, 2 picos, 3 ruinas
        std::uint8_t falls = 0;                 // cascadas que salen del lago (0..2)
        std::uint8_t caves = 0, tunnels = 0;
        bool golden = false, satellite = false;
        float lake_x = 0.0f, lake_z = 0.0f, lake_r = 0.0f, lake_depth = 4.0f;  // lake_r 0: sin lago
        int lake_level = 0;                     // ultimo bloque de agua del lago
        float fall_angle[2] = {0.0f, 0.0f};
        float ruin_x = 0.0f, ruin_z = 0.0f, ruin_r = 0.0f;  // ruin_r 0: sin ruinas
        int ruin_level = 0;
        // Cuevas: camaras (elipsoides; kind 0 gruta de cristal, 1 lago, 2 boca al cielo) y tuneles.
        struct Cave {
            float x = 0.0f, y = 0.0f, z = 0.0f, rx = 1.0f, ry = 1.0f, rz = 1.0f;
            int kind = 0, water = 0;  // water: ultimo bloque de agua (camara del lago)
        };
        struct Tunnel {
            float ax = 0.0f, ay = 0.0f, az = 0.0f, bx = 0.0f, by = 0.0f, bz = 0.0f, r = 2.5f;
        };
        Cave cave[4];
        Tunnel tunnel[5];
    };
    // Una columna de una isla: macizo de bottom a top (ultimo bloque); water >
    // top: agua del lago hasta ahi; t: 0 en el centro .. 1 en la costa.
    struct WildIslandColumn {
        int bottom = 0, top = 0, water = 0;
        float t = 1.0f;
        bool rock = false;  // cima de roca (picos)
    };
    bool wildIslandColumn(const WildIsland& isl, int x, int z, WildIslandColumn& out) const;
    bool wildIslandSpan(const WildIsland& isl, int x, int z, int& bottom, int& top, float& t) const;
    // Las islas cuya zona toca el rectangulo [x0, x1] x [z0, z1] (+ margen).
    // (`small` ya no se usa: no hay islas pequenas.)
    void wildIslandsNear(int x0, int z0, int x1, int z1, bool small, std::vector<WildIsland>& out) const;
    // Las islas de una celda de la rejilla de islas (la grande y sus satelites; hasta 4).
    int wildIslandCell(int gx, int gz, WildIsland* out) const;
    // Islas, agujas, volcanes... lo que no es del mapa de alturas (un chunk ya relleno).
    // `heights`: el suelo de las columnas (x + z * 16).
    void placeWild(ChunkData& chunk, const std::array<int, kChunkSize * kChunkSize>& heights) const;
private:
    // Dimensiones (GeneratorDimensions.cpp): abismo, cielo y cristal.
    struct AbyssColumn;
    struct SkyIsland;
    AbyssColumn abyssColumn(int x, int z) const;
    bool skyIsland(int gx, int gz, bool big, SkyIsland& out) const;
    bool skySpan(const SkyIsland& island, int x, int z, int& bottom, int& top, float& t) const;
    int skyTop(int x, int z, float* t = nullptr) const;
    int crystalHeight(int x, int z) const;
    int dimensionHeight(int x, int z) const;
    void generateAbyss(ChunkData& chunk) const;
    void generateSky(ChunkData& chunk) const;
    void generateCrystal(ChunkData& chunk) const;

    int seed_ = 0;
    int sea_ = 62;
    float tree_density_ = 1.0f;
    float clearings_ = 0.0f;
    int cave_entrances_ = 1;
    bool immense_caves_ = true;
    bool deep_oceans_ = true;
    bool arcanite_ore_ = true;
    bool lush_biomes_ = true;  // NOCHE-MUNDO
    bool nature_ = kNaturaDefault;  // NATURALEZA-HZ
    std::shared_ptr<EntranceCache> entrances_;
    VoxelGenerator profile_ = VoxelGenerator::Classic;
    std::array<int, 512> perm_{};
    std::shared_ptr<Structures> structures_;
};

// Lagos ya calculados (los comparten los hilos que generan).
struct Generator::LakeCache {
    std::uint64_t id = 0;  // unico por generador (la cache por hilo no confunde generadores)
    std::mutex mutex;
    std::unordered_map<std::int64_t, Lake> cells;
};

// Entradas de cueva ya calculadas por celda (nulo: la celda no tiene).
struct Generator::EntranceCache {
    std::mutex mutex;
    std::unordered_map<std::int64_t, std::shared_ptr<const CaveEntrance>> cells;
};

// --- Mallador ------------------------------------------------------------------------

// Bloques y luz de una seccion con un borde de 1 (18^3), copiados del mundo
// en el hilo principal para mallar en otro hilo.
struct SectionSnapshot {
    static constexpr int kSize = kChunkSize + 2;
    int cx = 0, sy = 0, cz = 0;
    std::array<BlockId, kSize * kSize * kSize> blocks{};
    std::array<std::uint8_t, kSize * kSize * kSize> light{};
    std::array<std::uint16_t, kSize * kSize> grass_tint{};
    std::array<std::uint16_t, kSize * kSize> foliage_tint{};
    // MALLAS-DETALLE: nivel de adornos de esta seccion (meshTierFor): 2 todo (como siempre), 1 sin hojas que
    // cuelgan, pepitas de mena sencillas y matas de 2 laminas, 0 ademas sin lamina esponjosa de las hojas, sin
    // pepitas, matas de 1 lamina y luz plana (sin oclusion de esquina: casi todas las caras se fusionan). Los
    // bloques y sus caras macizas son los mismos en los tres: sin agujeros entre secciones ni cambios de colision.
    std::uint8_t mesh_tier = 2;

    static int index(int x, int y, int z) { return (x + 1) + (z + 1) * kSize + (y + 1) * kSize * kSize; }
};

// Caras de la seccion (VoxelPass: 2 uint32 por vertice, 4 por cara). `water`
// (si no es nulo): las caras del agua aparte (formato de water_voxel.vert:
//   a: x (5) | y (5) | z (5) | cara (3) | altura en dieciseisavos sobre y (5) |
//      superficie libre (1) | corriente x (4, +8) | corriente z (4, +8)
//   b: luz del cielo (4) | luz de bloque (4) | cae (1)).
// MALLAS-DETALLE: lo que el nivel de adornos cambia en una seccion (para no volver a mallarla si da igual).
enum MeshTierFlags : std::uint8_t {
    kTierDecor = 1,  // hay adornos (matas, hojas esponjosas, menas en relieve): cambia entre los niveles 1 y 2
    kTierFaces = 2,  // hay caras con luz suave: cambia entre los niveles 0 y 1
};
void meshSection(const SectionSnapshot& snapshot, std::vector<std::uint32_t>& out,
                 std::vector<std::uint32_t>* water = nullptr, std::uint8_t* tier_flags = nullptr);
// MALLAS-DETALLE: CVar voxel.mesh.Detail (0 Bajo, 1 Medio, 2 Alto) y el nivel de adornos de una seccion cuyo
// punto mas cercano esta a `distance` m de quien mira. Alto: 2 siempre. Medio: 2 hasta 48 m, 1 hasta 96, luego 0.
// Bajo: 2 hasta 16 m, 1 hasta 40, luego 0.
int meshDetailLevel();
int meshTierFor(int level, float distance);
// Altura de la superficie de un bloque de agua en dieciseisavos (fuente 14,
// como Minecraft; la corriente mas baja cuanto mas lejos de la fuente).
int waterHeight16(int level);

// Tabla compartida por el mallador, el shader y las pruebas: normal de cada
// cara y sus ejes u (derecha) y v (abajo en la textura).
struct FaceAxes {
    int n[3];
    int u[3];
    int v[3];
};
extern const std::array<FaceAxes, 6> kFaces;

// COLISION-MODELOS (2026-11-09): cajas con las que chocan los bloques SOLIDOS con modelo propio
// (vallas, mesas, sillas, bancos, braseros y postes de farola). Antes chocaban como un cubo entero
// aunque se dibujaran finos. Las usan VoxelSystem::boxCollides / moveBox, la malla de colision de la
// fisica (collisionTriangles) y raycast (lo que se apunta y su caja: boxMin / boxMax). Siguen las mismas
// reglas de vecinos que el mallador (Mesher.cpp): travesanos de las vallas, patas de las mesas juntas,
// respaldo de la silla, eje del banco. Dieciseisavos de la celda. 0 cajas = cubo entero (o no choca).
struct ModelBox {
    std::uint8_t lo[3];
    std::uint8_t hi[3];
};
using ModelBoxes = std::array<ModelBox, 8>;

inline bool hasModelCollision(const BlockDef& def) {
    if (!def.solid || def.shape != BlockShape::Cube) return false;
    switch (def.model) {
        case BlockModel::Fence:
        case BlockModel::Table:
        case BlockModel::Chair:
        case BlockModel::Bench:
        case BlockModel::Brazier:
        case BlockModel::Post:
            return true;
        default:
            return false;
    }
}

template <class BlockAt>
int modelCollisionBoxes(BlockId id, const BlockAt& block_at, int x, int y, int z, ModelBoxes& out) {
    const BlockDef& def = blockDef(id);
    if (!hasModelCollision(def)) return 0;
    int n = 0;
    const auto add = [&](int x0, int y0, int z0, int x1, int y1, int z1) {
        if (n >= static_cast<int>(out.size())) return;
        out[static_cast<std::size_t>(n++)] = ModelBox{{static_cast<std::uint8_t>(x0), static_cast<std::uint8_t>(y0), static_cast<std::uint8_t>(z0)},
                                                      {static_cast<std::uint8_t>(x1), static_cast<std::uint8_t>(y1), static_cast<std::uint8_t>(z1)}};
    };
    // Vecinos en las caras laterales (orden de kFaces): 0 +X, 1 -X, 4 +Z, 5 -Z.
    const auto nb = [&](int f) {
        const int dx = f == 0 ? 1 : (f == 1 ? -1 : 0);
        const int dz = f == 4 ? 1 : (f == 5 ? -1 : 0);
        return block_at(x + dx, y, z + dz);
    };
    switch (def.model) {
        case BlockModel::Fence: {
            add(6, 0, 6, 10, 16, 10);  // poste
            // Travesanos (las dos alturas en una caja) hacia vallas y bloques macizos, como Mesher.cpp.
            for (const int f : {0, 1, 4, 5}) {
                const BlockId other = nb(f);
                const BlockDef& od = blockDef(other);
                if (other != id && !(od.opaque && od.solid && od.shape == BlockShape::Cube)) continue;
                if (f == 0) add(10, 5, 7, 16, 13, 9);
                if (f == 1) add(0, 5, 7, 6, 13, 9);
                if (f == 4) add(7, 5, 10, 9, 13, 16);
                if (f == 5) add(7, 5, 0, 9, 13, 6);
            }
            return n;
        }
        case BlockModel::Table: {
            add(0, 14, 0, 16, 16, 16);  // tablero
            for (const int cx : {0, 1}) {
                for (const int cz : {0, 1}) {
                    if (nb(cx ? 0 : 1) == id || nb(cz ? 4 : 5) == id) continue;
                    const int x0 = cx ? 13 : 1, z0 = cz ? 13 : 1;
                    add(x0, 0, z0, x0 + 2, 14, z0 + 2);  // pata
                }
            }
            return n;
        }
        case BlockModel::Chair: {
            // Respaldo lejos de la mesa de al lado; si no, contra la pared; si no, a -Z (Mesher.cpp).
            int back = 5;
            bool found = false;
            for (const int f : {0, 1, 4, 5}) {
                if (blockDef(nb(f)).model == BlockModel::Table) {
                    back = f ^ 1;
                    found = true;
                    break;
                }
            }
            if (!found) {
                for (const int f : {0, 1, 4, 5}) {
                    const BlockDef& d = blockDef(nb(f));
                    if (d.solid && d.opaque) {
                        back = f;
                        break;
                    }
                }
            }
            const auto part = [&](int x0, int y0, int z0, int x1, int y1, int z1) {
                const auto map = [&](int px, int pz, int& ox, int& oz) {
                    switch (back) {
                        case 4: ox = px; oz = 16 - pz; break;
                        case 1: ox = pz; oz = px; break;
                        case 0: ox = 16 - pz; oz = px; break;
                        default: ox = px; oz = pz; break;
                    }
                };
                int ax = 0, az = 0, bx = 0, bz = 0;
                map(x0, z0, ax, az);
                map(x1, z1, bx, bz);
                add(std::min(ax, bx), y0, std::min(az, bz), std::max(ax, bx), y1, std::max(az, bz));
            };
            part(3, 0, 3, 13, 9, 13);   // asiento con las patas
            part(3, 9, 3, 13, 16, 5);   // respaldo
            return n;
        }
        case BlockModel::Bench: {
            bool along_x = true;
            if (nb(0) == id || nb(1) == id) along_x = true;
            else if (nb(4) == id || nb(5) == id) along_x = false;
            else if (blockDef(nb(0)).model == BlockModel::Table || blockDef(nb(1)).model == BlockModel::Table) along_x = false;
            if (along_x) {
                add(0, 6, 3, 16, 8, 13);
                if (nb(1) != id) add(1, 0, 4, 3, 6, 12);
                if (nb(0) != id) add(13, 0, 4, 15, 6, 12);
            } else {
                add(3, 6, 0, 13, 8, 16);
                if (nb(5) != id) add(4, 0, 1, 12, 6, 3);
                if (nb(4) != id) add(4, 0, 13, 12, 6, 15);
            }
            return n;
        }
        case BlockModel::Brazier: {
            add(4, 0, 4, 12, 1, 12);   // pie
            add(7, 1, 7, 9, 6, 9);     // columna
            add(3, 6, 3, 13, 10, 13);  // cuenco (las llamas no chocan)
            return n;
        }
        case BlockModel::Post: {
            add(5, 0, 5, 11, 16, 11);  // poste de farola (base y remate incluidos)
            return n;
        }
        default:
            return 0;
    }
}

}  // namespace cramion::voxel

#endif  // CRAMION_CORE_VOXEL_INTERNAL_H
