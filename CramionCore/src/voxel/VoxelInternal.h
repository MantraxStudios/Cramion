#ifndef CRAMION_CORE_VOXEL_INTERNAL_H
#define CRAMION_CORE_VOXEL_INTERNAL_H

// Piezas internas del mundo de bloques: la columna (chunk), el generador y
// el mallador. VoxelSystem.cpp las une.

#include "CramionCore/voxel/Voxel.h"

#include <array>
#include <cstdint>
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

class Generator {
public:
    Generator(int seed, int sea_level);
    // Rellena la columna (bloques y tintes).
    void generate(ChunkData& chunk) const;
    // Altura del terreno (sin cuevas ni arboles): el ultimo bloque solido.
    int terrainHeight(int x, int z) const;
    int seaLevel() const { return sea_; }

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

    int seed_ = 0;
    int sea_ = 62;
    std::array<int, 512> perm_{};
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

// Caras de la seccion (VoxelPass: 2 uint32 por vertice, 4 por cara).
void meshSection(const SectionSnapshot& snapshot, std::vector<std::uint32_t>& out);

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
