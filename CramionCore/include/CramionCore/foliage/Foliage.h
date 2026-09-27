#ifndef CRAMION_CORE_FOLIAGE_H
#define CRAMION_CORE_FOLIAGE_H

// Vegetacion (como el Foliage de Unreal o los arboles del Terrain de Unity):
// un componente que siembra MILLONES de arboles en un cuadrado alrededor de su
// entidad. No crea entidades: las posiciones se generan (en varios hilos) y el
// renderizador las dibuja instanciadas con recorte y niveles de detalle en la
// GPU (gfx::FoliagePass).
//
//   - Siembra en rejilla con desorden: un arbol por celda (el tamano de la
//     celda sale de la densidad), en un punto al azar de ella.
//   - Sobre el terreno que haya debajo (su altura), sin pasar de la pendiente
//     maxima ni bajar de la altura minima (el mar, la playa).
//   - Tres especies (pino, roble y abedul) mezcladas con los pesos dados, con
//     giro, escala y tono al azar. Misma semilla = mismo bosque.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/terrain/Terrain.h"

#include <CramionFX/vk/FoliagePass.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace cramion::foliage {

// Zona sin arboles (poblados, caminos, el punto de partida): un circulo en
// el suelo, en coordenadas del mundo.
struct FoliageClearing {
    core::Vec3 center{};
    float radius = 20.0f;
};

struct Foliage {
    float area = 2000.0f;     // lado del cuadrado sembrado (m), centrado en la entidad
    float density = 60.0f;    // arboles por hectarea (100 x 100 m)
    int seed = 1;
    float pine = 0.5f;        // mezcla de especies (pesos)
    float oak = 0.3f;
    float birch = 0.2f;
    float min_scale = 0.8f;
    float max_scale = 1.35f;
    bool on_terrain = true;   // apoyarlos en el terreno de debajo
    float min_height = 1.5f;  // altura del mundo minima (por encima del mar)
    float max_height = 100000.0f;
    float max_slope = 32.0f;  // grados
    int max_instances = 4000000;
    std::vector<FoliageClearing> clearings;
    // Dibujo (gfx::FoliageSettings).
    float lod1_distance = 120.0f;
    float lod2_distance = 450.0f;
    float max_distance = 3000.0f;
    float shadow_distance = 140.0f;
    bool cast_shadows = true;
    float wind = 1.0f;

    void reflect(ecs::PropertyVisitor& v);
    gfx::FoliageSettings settings() const;
};

// Un terreno donde apoyar la vegetacion (sus datos se leen, no se copian).
struct FoliageGround {
    std::shared_ptr<const terrain::TerrainData> data;
    terrain::Terrain terrain;
    core::Vec3 origin{};  // esquina (x, z minimas) y altura 0
};

struct FoliageResult {
    std::vector<gfx::FoliageInstance> instances;
    std::uint64_t candidates = 0;  // celdas probadas
    std::uint64_t rejected = 0;    // por pendiente, altura o fuera del terreno
    float cell = 0.0f;             // lado de la celda usada (m)
};

// Siembra (en paralelo). `center` = posicion de la entidad.
FoliageResult generateFoliage(const Foliage& foliage, const core::Vec3& center, const std::vector<FoliageGround>& ground);

void registerFoliageComponents();

}  // namespace cramion::foliage

#endif  // CRAMION_CORE_FOLIAGE_H
