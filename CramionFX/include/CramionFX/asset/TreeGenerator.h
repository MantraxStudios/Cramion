#ifndef CRAMION_ASSET_TREE_GENERATOR_H
#define CRAMION_ASSET_TREE_GENERATOR_H

// Arboles procedurales (como SpeedTree, en pequeno): cada especie es un
// esqueleto de ramas generado por reglas (tronco, ramas en espiral de 137.5
// grados, subdivisiones, gravedad, fototropismo y torsion) convertido en
// tubos de corteza con UV y tarjetas de hojas con transparencia recortada.
//
//   - 6 tipos: pino, abeto, roble, abedul, palmera y sauce, con sus reglas.
//   - Los parametros de la especie (altura, densidad de hojas y ramas,
//     torsion, colores, semilla) cambian el arbol sin tocar codigo.
//   - 3 niveles de detalle del MISMO esqueleto (la silueta no salta): menos
//     lados y menos ramas finas lejos, con hojas mas grandes y menos.
//   - Normales "esfericas" en las hojas (de la copa hacia fuera): la copa se
//     ilumina como un volumen, no como tarjetas sueltas.
//   - Texturas procedurales (corteza, corteza de abedul, racimo de hojas,
//     aguja de pino y fronda de palmera) con mipmaps que conservan la
//     cobertura del recorte (las hojas no desaparecen de lejos).

#include "CramionFX/core/Math.h"

#include <cstdint>
#include <vector>

namespace cramion::asset {

enum class TreeKind : int { Pine = 0, Fir, Oak, Birch, Palm, Willow };
inline constexpr int kTreeKindCount = 6;
const char* treeKindName(TreeKind kind);

struct TreeSpecies {
    TreeKind kind = TreeKind::Oak;
    std::uint32_t seed = 1;
    float height = 11.0f;          // metros (a escala 1)
    float leaf_density = 1.0f;     // multiplica las hojas
    float leaf_size = 1.0f;        // multiplica el tamano de las hojas
    float branch_density = 1.0f;   // multiplica las ramas
    float gnarl = 1.0f;            // multiplica lo torcido de las ramas
    core::Vec3 leaf_color{0.3f, 0.45f, 0.14f};   // sRGB
    core::Vec3 bark_color{0.36f, 0.28f, 0.2f};   // sRGB

    bool operator==(const TreeSpecies& o) const;
};

// Valores de fabrica de cada tipo.
TreeSpecies treePreset(TreeKind kind);

// Capas del array de texturas de los arboles.
inline constexpr std::uint32_t kTreeLayerBark = 0;
inline constexpr std::uint32_t kTreeLayerLeaves = 1;
inline constexpr std::uint32_t kTreeLayerNeedles = 2;
inline constexpr std::uint32_t kTreeLayerFrond = 3;
inline constexpr std::uint32_t kTreeLayerBirchBark = 4;
inline constexpr std::uint32_t kTreeLayerCount = 5;

// Vertice de los arboles (48 bytes).
struct TreeVertex {
    float px, py, pz;
    float nx, ny, nz;
    std::uint32_t color;  // RGBA8: rgb color (sRGB), a oclusion
    float wind;           // 0 = rigido (base del tronco), 1 = punta de la copa
    float u, v;
    float layer;          // capa de textura (kTreeLayer*)
    float flutter;        // 0 corteza, 1 hoja (tiembla con el viento)
};
static_assert(sizeof(TreeVertex) == 48, "TreeVertex debe coincidir con foliage.vert");

struct TreeMeshData {
    std::vector<TreeVertex> vertices;
    std::vector<std::uint32_t> indices;
    float height = 10.0f;
    // Esfera que lo envuelve (a escala 1): centro sobre el pie (x = z = 0) y radio.
    float center_y = 5.0f;
    float radius = 6.0f;
};

// `lod`: 0 cerca, 1 media, 2 lejos.
TreeMeshData buildTree(const TreeSpecies& species, int lod);

// Texturas de los arboles: kTreeLayerCount capas de `size` x `size` con todos
// sus mipmaps. `albedo[capa][nivel]` y `normal[capa][nivel]`, RGBA8.
struct TreeTextures {
    std::uint32_t size = 0;
    std::uint32_t mips = 0;
    std::vector<std::vector<std::vector<std::uint8_t>>> albedo;
    std::vector<std::vector<std::vector<std::uint8_t>>> normal;
};
TreeTextures generateTreeTextures(std::uint32_t size = 512);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_TREE_GENERATOR_H
