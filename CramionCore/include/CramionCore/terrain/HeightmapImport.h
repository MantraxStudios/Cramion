#ifndef CRAMION_CORE_TERRAIN_HEIGHTMAP_IMPORT_H
#define CRAMION_CORE_TERRAIN_HEIGHTMAP_IMPORT_H

// Importar mapas de alturas (los de World Machine, Gaea, QGIS, Unity o
// Unreal) a un terreno o a una rejilla de teselas (TerrainGrid):
//
//   .raw / .r16   16 bits sin signo, little endian, cuadrado (el lado sale
//                 del tamano del archivo)
//   .r32          float de 32 bits, cuadrado (si no esta en 0..1 se
//                 normaliza entre su minimo y su maximo)
//   .png .tga ... PNG de 16 bits en gris (o de 8 bits, con menos detalle)
//
// La rejilla se escribe tesela a tesela (nunca el mundo entero en memoria):
// cada vertice se muestrea del mapa con un filtro bicubico, asi que los
// bordes compartidos salen identicos en las dos teselas (sin grietas) y un
// mapa pequeno se amplia suave, sin escalones.

#include "CramionCore/terrain/TerrainGrid.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cramion::terrain {

struct Heightmap {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> values;  // 0..1, fila 0 = z minima
    // Lo que valian el 0 y el 1 en el archivo (r32 en metros, por ejemplo).
    float source_min = 0.0f;
    float source_max = 1.0f;
    bool valid() const { return width >= 2 && height >= 2 && values.size() == static_cast<std::size_t>(width) * height; }
};

bool loadHeightmap(const std::filesystem::path& file, Heightmap& out, std::string* error = nullptr);
// Las alturas de un terreno ya hecho (para pasarlo a una rejilla).
Heightmap heightmapFromTerrain(const TerrainData& data);
// Altura 0..1 en uv (0..1), bicubica (Catmull-Rom), con los bordes fijos.
float sampleHeightmap(const Heightmap& map, float u, float v);
void flipHeightmapZ(Heightmap& map);

// Todo el mapa en un terreno (remuestreado a su resolucion).
void importIntoTerrain(const Heightmap& map, TerrainData& data);

struct GridImportOptions {
    bool auto_paint = true;   // roca en las pendientes, el resto la capa 0
    int rock_layer = 2;
    float rock_slope = 35.0f;  // grados
    bool overwrite = true;    // si no, las teselas que ya existen se saltan
};

struct GridImportProgress {
    std::atomic<int> done{0};
    std::atomic<int> total{0};
    std::atomic<bool> cancel{false};
};

// Escribe las tiles_x * tiles_z teselas de `grid` en <assets_root>/<folder>.
// Se puede llamar desde otro hilo. false si no se pudo (con `error`).
bool writeTerrainGrid(const Heightmap& map, const TerrainGrid& grid, const std::filesystem::path& assets_root,
                      const GridImportOptions& options, GridImportProgress* progress = nullptr,
                      std::string* error = nullptr);

}  // namespace cramion::terrain

#endif  // CRAMION_CORE_TERRAIN_HEIGHTMAP_IMPORT_H
