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
#include <CramionFX/vk/TerrainPass.h>

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

// Agua donde no crecen arboles (la pone RenderSync con los rios y lagos de la
// escena; no se guarda). Rio: tramo de `a` a `b` con su anchura. Lago:
// rectangulo (centro `a`, medio tamano `half`, giro `angle`) a su nivel: solo
// se quitan los que quedan por debajo del agua (en la orilla si crecen).
struct FoliageWater {
    bool river = false;
    core::Vec3 a{};
    core::Vec3 b{};
    float width = 0.0f;
    core::Vec2 half{};
    float angle = 0.0f;
    float level = 0.0f;
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
    std::vector<FoliageWater> water;  // en marcha (no se guarda)
    // Dibujo (gfx::FoliageSettings).
    float lod1_distance = 120.0f;
    float lod2_distance = 450.0f;
    float max_distance = 3000.0f;
    float shadow_distance = 140.0f;
    bool cast_shadows = true;
    float wind = 1.0f;

    // Especies (arboles procedurales): tipo de cada una de las 3 (pesos de
    // arriba: pine/oak/birch = especie 1/2/3) y como son.
    int species1 = 0;   // asset::TreeKind (0 pino, 1 abeto, 2 roble, 3 abedul, 4 palmera, 5 sauce)
    int species2 = 2;
    int species3 = 3;
    int tree_seed = 1;
    float tree_height = 1.0f;     // multiplica la altura de fabrica
    float leaf_density = 1.0f;
    float branch_density = 1.0f;
    float gnarl = 1.0f;

    void reflect(ecs::PropertyVisitor& v);
    gfx::FoliageSettings settings() const;
    std::array<asset::TreeSpecies, gfx::FoliagePass::kSpecies> species() const;
};

// Hierba (en la misma entidad que un Terreno): millones de briznas generadas
// en la GPU alrededor de la camara donde el terreno tiene su capa de hierba.
// Se mecen con el viento, brillan a contraluz y se apartan y aplastan con los
// objetos fisicos y los personajes. Sin coste en la CPU ni en memoria por brizna.
struct Grass {
    int layer = 0;               // capa del terreno donde crece
    float threshold = 0.25f;     // peso minimo de esa capa (0..1)
    int dry_layer = 1;           // capa que la seca (-1 = ninguna)
    float density = 70.0f;       // briznas por m2
    float max_distance = 80.0f;  // m
    float detail_distance = 22.0f;  // briznas con detalle hasta aqui (m)
    float height = 0.45f;        // m
    float height_variation = 0.4f;
    float width = 0.028f;        // m
    float bend = 0.35f;
    core::Vec3 base_color{0.06f, 0.11f, 0.03f};
    core::Vec3 tip_color{0.27f, 0.38f, 0.11f};
    core::Vec3 dry_color{0.5f, 0.45f, 0.25f};
    float color_variation = 0.25f;
    float wind = 1.0f;
    float wind_direction = 30.0f;  // grados
    float interaction = 1.0f;      // cuanto la apartan los objetos
    int max_blades = 3000000;

    void reflect(ecs::PropertyVisitor& v);
    gfx::GrassDesc desc() const;
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
