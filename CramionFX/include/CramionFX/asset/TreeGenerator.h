#ifndef CRAMION_ASSET_TREE_GENERATOR_H
#define CRAMION_ASSET_TREE_GENERATOR_H

// Arboles procedurales (como SpeedTree): cada especie es un esqueleto de
// ramas generado por reglas (tronco, ramas en espiral de 137.5 grados,
// subdivisiones, gravedad, fototropismo y curvas suaves) convertido en
// troncos y ramas organicos y en racimos de hojas que siguen a las ramitas.
//
//   - 6 tipos: pino, abeto, roble, abedul, palmera y sauce, con sus reglas,
//     su forma de copa y sus texturas propias (corteza y hojas).
//   - Tronco con raices: ensanche del pie, contrafuertes y raices que se
//     hunden en el suelo; seccion irregular (no un tubo perfecto), curvas de
//     Catmull-Rom, cuello en el nacimiento de cada rama, ramas secas rotas.
//   - Hojas en tarjetas CURVADAS (varias filas, dobladas por el nervio y
//     caidas por la gravedad) que siguen a las ramitas; agujas en penachos
//     cruzados (pino) o en capas planas (abeto); cortinas colgantes (sauce);
//     frondas con pliegue en V y hojas secas (palmera).
//   - Oclusion de la copa calculada de verdad: una rejilla de densidad de
//     hojas y rayos hacia el cielo (el interior de la copa queda oscuro).
//   - Datos de viento jerarquico en cada vertice: tronco, rama principal
//     (gira sobre su pivote) y temblor de la hoja.
//   - 3 niveles de detalle del MISMO esqueleto y de las mismas hojas (la
//     silueta no salta): menos lados, menos filas y menos tarjetas lejos.
//   - Texturas procedurales a 1024 px (color + normal, oclusion, rugosidad
//     o translucidez) con mipmaps que conservan la cobertura del recorte.

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
    // Tintes sobre el color de las texturas (1 = el de la textura).
    core::Vec3 leaf_color{1.0f, 1.0f, 1.0f};
    core::Vec3 bark_color{1.0f, 1.0f, 1.0f};

    bool operator==(const TreeSpecies& o) const;
};

// Valores de fabrica de cada tipo.
TreeSpecies treePreset(TreeKind kind);

// Capas del array de texturas de los arboles.
inline constexpr std::uint32_t kTreeLayerBark = 0;         // roble / sauce (surcos y crestas)
inline constexpr std::uint32_t kTreeLayerLeaves = 1;       // racimo de hojas de roble
inline constexpr std::uint32_t kTreeLayerNeedles = 2;      // penacho de agujas de pino
inline constexpr std::uint32_t kTreeLayerFrond = 3;        // fronda de palmera
inline constexpr std::uint32_t kTreeLayerBirchBark = 4;    // abedul (blanca con lenticelas)
inline constexpr std::uint32_t kTreeLayerPineBark = 5;     // pino / abeto (placas)
inline constexpr std::uint32_t kTreeLayerBirchLeaves = 6;  // ramita de abedul
inline constexpr std::uint32_t kTreeLayerWillowLeaves = 7; // cordon colgante de sauce
inline constexpr std::uint32_t kTreeLayerFirNeedles = 8;   // rama plana de abeto
inline constexpr std::uint32_t kTreeLayerPalmBark = 9;     // anillos del estipe
inline constexpr std::uint32_t kTreeLayerCount = 10;

// Vertice de los arboles (64 bytes).
struct TreeVertex {
    float px, py, pz;
    float nx, ny, nz;
    std::uint32_t color;  // RGBA8: rgb tinte x 0.5 (sRGB; 0.5 = textura tal cual), a oclusion
    float wind;           // balanceo del tronco: 0 = rigido (pie), 1 = punta de la copa
    float u, v;
    float layer;          // capa de textura (kTreeLayer*)
    float flutter;        // 0 corteza; hoja: cuanto tiembla (0 donde nace la tarjeta .. 1 punta)
    float bx, by, bz;     // pivote de la rama principal que lo lleva (donde nace del tronco)
    // RGBA8: r fase de la rama, g flexibilidad de la rama, b fase de la hoja,
    // a extra (corteza: musgo / pie oscuro del abedul; hoja: translucidez).
    std::uint32_t anim;
};
static_assert(sizeof(TreeVertex) == 64, "TreeVertex debe coincidir con foliage.vert");

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
// sus mipmaps. `albedo[capa][nivel]` (RGBA8 sRGB: color y alfa) y
// `normal[capa][nivel]` (RGBA8: rg normal xy, b oclusion, a rugosidad en la
// corteza / translucidez en las hojas).
struct TreeTextures {
    std::uint32_t size = 0;
    std::uint32_t mips = 0;
    std::vector<std::vector<std::vector<std::uint8_t>>> albedo;
    std::vector<std::vector<std::vector<std::uint8_t>>> normal;
};
TreeTextures generateTreeTextures(std::uint32_t size = 1024);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_TREE_GENERATOR_H
