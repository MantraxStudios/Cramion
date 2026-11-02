#ifndef CRAMION_CORE_VOXEL_INTERNAL_H
#define CRAMION_CORE_VOXEL_INTERNAL_H

// Piezas internas del mundo de bloques: la columna (chunk), el generador y
// el mallador. VoxelSystem.cpp las une.

#include "CramionCore/voxel/Voxel.h"

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

    BlockId get(int x, int y, int z) const { return blocks[static_cast<std::size_t>(cellIndex(x, y, z))]; }
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
    // Nivel de los mares de lava del abismo.
    static constexpr int kAbyssLava = 34;
    VoxelGenerator profile() const { return profile_; }
    // Aldeas, ciudades, mazmorras... (Structures.h); nulo si el generador no tiene.
    const Structures* structures() const { return structures_.get(); }

    // Biomas del generador Aventura (pruebas y depuracion).
    enum AdventureBiome : int { AOcean = 0, ABeach, ARiver, AMeadow, AForest, ADesert, ASnow, AMountains };
    struct AdventureColumn {
        int height = 64;
        float temperature = 0.0f;  // -1 frio .. 1 caluroso
        float humidity = 0.0f;     // -1 seco .. 1 humedo
        float river = 0.0f;        // 0 lejos .. 1 en el cauce
        bool mesa = false;         // meseta del desierto (terracota)
        int biome = AMeadow;
        int water = 0;             // primer bloque por encima del agua (mar o lago); <= height: seca
        bool lake = false;         // en un lago (o su orilla)
    };
    AdventureColumn adventureColumn(int x, int z) const;

private:
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
    void placeAdventureTrees(ChunkData& chunk) const;
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

    static int index(int x, int y, int z) { return (x + 1) + (z + 1) * kSize + (y + 1) * kSize * kSize; }
};

// Caras de la seccion (VoxelPass: 2 uint32 por vertice, 4 por cara). `water`
// (si no es nulo): las caras del agua aparte (formato de water_voxel.vert:
//   a: x (5) | y (5) | z (5) | cara (3) | altura en dieciseisavos sobre y (5) |
//      superficie libre (1) | corriente x (4, +8) | corriente z (4, +8)
//   b: luz del cielo (4) | luz de bloque (4) | cae (1)).
void meshSection(const SectionSnapshot& snapshot, std::vector<std::uint32_t>& out,
                 std::vector<std::uint32_t>* water = nullptr);
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

}  // namespace cramion::voxel

#endif  // CRAMION_CORE_VOXEL_INTERNAL_H
