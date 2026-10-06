#ifndef CRAMION_CORE_TERRAIN_TOOLS_H
#define CRAMION_CORE_TERRAIN_TOOLS_H

// Herramientas de terreno (las del modo Landscape de Unreal). Trabajan sobre
// TerrainData en el espacio del mundo: `origin` es la posicion de la entidad
// del terreno (esquina x, z minimas y altura 0).

#include "CramionCore/terrain/Terrain.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <vector>

namespace cramion::terrain {

enum class TerrainTool : int {
    Raise = 0,   // subir (Mayus: bajar)
    Lower,       // bajar
    Smooth,      // suavizar
    Flatten,     // aplanar a una altura (Ctrl+clic la toma del terreno)
    Ramp,        // rampa entre dos puntos
    Noise,       // ruido (relieve irregular)
    Erosion,     // erosion termica (se desmoronan las pendientes fuertes)
    Hydraulic,   // erosion por agua (cauces, sedimentos)
    Terrace,     // terrazas (escalones)
    Paint,       // pintar una capa (Mayus: borrarla)
};
inline constexpr int kTerrainToolCount = 10;
const char* toolName(TerrainTool tool);

struct TerrainBrush {
    TerrainTool tool = TerrainTool::Raise;
    float radius = 10.0f;     // metros
    float strength = 0.5f;    // 0..1
    float falloff = 0.5f;     // 0 = borde duro, 1 = todo suave
    float target_height = 0.0f;  // aplanar (altura del mundo)
    float noise_scale = 0.08f;
    std::uint32_t seed = 1337;
    int layer = 0;            // pintar
    float terrace_step = 4.0f;  // metros entre escalones
    float terrace_sharpness = 0.8f;
    float ramp_width = 6.0f;
};

// Peso del pincel a una distancia (0 fuera, 1 en el centro).
float brushWeight(const TerrainBrush& brush, float distance);

// Un toque del pincel en `center` (mundo) durante `dt` segundos. `invert` =
// la accion contraria (bajar, borrar la capa...). true si cambio algo.
bool applyBrush(TerrainData& data, const Terrain& terrain, const core::Vec3& origin, const core::Vec3& center,
                const TerrainBrush& brush, float dt, bool invert = false);

// Rampa de `a` a `b` (mundo) con el ancho y la caida del pincel.
bool applyRamp(TerrainData& data, const Terrain& terrain, const core::Vec3& origin, const core::Vec3& a,
               const core::Vec3& b, const TerrainBrush& brush);

// --- Huellas (edificios) y caminos ---
// Una huella es un rectangulo en el suelo girado en Y como su entidad:
// centro (mundo), medias medidas en X/Z locales y giro en grados (el de
// setLocalEulerDegrees({0, yaw, 0})).
struct Footprint {
    core::Vec3 center{};
    core::Vec2 half{1.0f, 1.0f};
    float yaw_degrees = 0.0f;
};

// Aplana el terreno bajo la huella a `height` (altura del mundo): dentro queda
// plano del todo (se cubren todos los vertices de las celdas que toca, para
// que el terreno no asome entre ellos) y en una franja de `blend` metros
// alrededor se mezcla suave con lo que habia (desmonte y terraplen).
// true si cambio algo.
bool flattenFootprint(TerrainData& data, const Terrain& terrain, const core::Vec3& origin, const Footprint& footprint,
                      float height, float blend);

// Altura del terreno bajo una huella: la mediana (sale tanta tierra como se
// rellena); min/max opcionales. Devuelve la altura del centro si cae fuera.
float footprintHeight(const TerrainData& data, const Terrain& terrain, const core::Vec3& origin,
                      const Footprint& footprint, float* min_height = nullptr, float* max_height = nullptr);

// Pinta una capa bajo la huella (suelo de tierra bajo una casa: alli no crece
// la hierba), con `blend` metros de borde suave. `amount` = peso final (0..1).
bool paintFootprint(TerrainData& data, const Terrain& terrain, const core::Vec3& origin, const Footprint& footprint,
                    int layer, float blend, float amount = 1.0f);

// Un camino: puntos en el mundo (su y = la altura del camino en ese punto).
// Allana el terreno a lo ancho (`width`) siguiendo la altura de los puntos y
// lo mezcla en `blend` metros a los lados.
bool flattenPath(TerrainData& data, const Terrain& terrain, const core::Vec3& origin,
                 const std::vector<core::Vec3>& points, float width, float blend);
// Pinta una capa a lo largo de un camino.
bool paintPath(TerrainData& data, const Terrain& terrain, const core::Vec3& origin, const std::vector<core::Vec3>& points,
               float width, int layer, float blend, float amount = 1.0f);

// Copia y restaura un trozo de las alturas (lo que se aplano bajo un objeto,
// para devolverlo al moverlo). Rectangulo de vertices inclusivo.
struct HeightPatch {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    std::vector<float> heights;
    bool valid() const { return x1 >= x0 && y1 >= y0; }
};
// Los vertices que tocaria flattenFootprint(footprint, blend).
HeightPatch captureFootprint(const TerrainData& data, const Terrain& terrain, const core::Vec3& origin,
                             const Footprint& footprint, float blend);
void restorePatch(TerrainData& data, const HeightPatch& patch);

// Relieve procedural en todo el terreno (fBm con crestas).
void generateRelief(TerrainData& data, std::uint32_t seed, float frequency, float roughness, float ridges,
                    float base_height);

// Rellena una capa en todo el terreno por pendiente y altura (roca en los
// taludes, nieve arriba...): 0..1 del terreno.
void paintByRules(TerrainData& data, const Terrain& terrain, int layer, float min_slope_deg, float max_slope_deg,
                  float min_height, float max_height);

// --- Consultas ---
float heightAt(const TerrainData& data, const Terrain& terrain, const core::Vec3& origin, float x, float z);
core::Vec3 normalAt(const TerrainData& data, const Terrain& terrain, const core::Vec3& origin, float x, float z);
bool raycast(const TerrainData& data, const Terrain& terrain, const core::Vec3& origin, const core::Vec3& ray_origin,
             const core::Vec3& ray_direction, float max_distance, core::Vec3& hit);

}  // namespace cramion::terrain

#endif  // CRAMION_CORE_TERRAIN_TOOLS_H
