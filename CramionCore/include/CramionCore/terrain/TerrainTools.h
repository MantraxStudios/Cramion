#ifndef CRAMION_CORE_TERRAIN_TOOLS_H
#define CRAMION_CORE_TERRAIN_TOOLS_H

// Herramientas de terreno (las del modo Landscape de Unreal). Trabajan sobre
// TerrainData en el espacio del mundo: `origin` es la posicion de la entidad
// del terreno (esquina x, z minimas y altura 0).

#include "CramionCore/terrain/Terrain.h"

#include <CramionFX/core/Math.h>

#include <cstdint>

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
