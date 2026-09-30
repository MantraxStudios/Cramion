#ifndef CRAMION_CORE_TERRAIN_GENERATOR_H
#define CRAMION_CORE_TERRAIN_GENERATOR_H

// Generador de terreno completo (como World Machine / Gaea, dentro del motor):
//
//   1. Forma: isla, archipielago, continente o cordilleras, con ruido fBm
//      deformado (domain warping), montanas de crestas (ridged multifractal)
//      que crecen tierra adentro, colinas y mesetas.
//   2. Erosion termica (se desmoronan las pendientes) e hidraulica por gotas
//      en todo el mapa (Beyer 2015), en paralelo por baldosas: cauces,
//      barrancos, abanicos de sedimentos.
//   3. Hidrologia: se rellenan las cuencas (priority-flood, Barnes 2014), se
//      calcula cuanta agua pasa por cada punto y los rios salen de la
//      desembocadura hacia arriba siguiendo el mayor caudal; se excavan con su
//      anchura y profundidad. Las cuencas hondas quedan como lagos.
//   4. Capas por reglas fisicas: hierba, hierba seca, tierra, roca, arena,
//      grava, nieve y barro segun pendiente, altura, humedad, sedimentos y
//      cercania al agua.
//
// El mar queda en y = 0 del mundo: la entidad del terreno va en
// origin = (-size / 2, -sea_level * height, -size / 2).

#include "CramionCore/terrain/Terrain.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cramion::terrain {

enum class GenShape : int { Island = 0, Archipelago, Continent, Mountains, Canyons };
inline constexpr int kGenShapeCount = 5;
const char* genShapeName(GenShape shape);

struct GenSettings {
    std::uint32_t seed = 1;
    GenShape shape = GenShape::Island;

    // --- Tamano ---
    float size = 2048.0f;     // metros por lado
    float height = 420.0f;    // metros del fondo del mar a la cumbre mas alta posible
    int resolution = 1025;    // vertices por lado (513, 1025, 2049)
    int splat_resolution = 1024;
    float sea_level = 0.16f;  // 0..1 de la altura (Cordilleras: fondo de los valles, sin mar)

    // --- Relieve ---
    float feature_scale = 1.0f;  // tamano de las formas (1 = montanas de ~1 km)
    float mountains = 0.65f;     // cuanto de la tierra son montanas
    float ridges = 0.7f;         // crestas afiladas (0 = redondeadas)
    float hills = 0.45f;         // colinas y ondulacion de las llanuras
    float warp = 0.55f;          // deformacion (formas menos "de ruido")
    float plateaus = 0.0f;       // mesetas y escalones
    float coast = 0.5f;          // costa irregular (bahias, cabos)

    // --- Erosion ---
    float erosion = 0.7f;          // gotas de lluvia (0 = nada, 1 = mucha)
    float erosion_strength = 0.5f; // cuanto arranca cada gota
    float thermal = 0.35f;         // desmoronamiento de las laderas
    float talus = 38.0f;           // grados a partir de los que se desmorona

    // --- Agua ---
    bool ocean = true;
    int rivers = 4;              // rios principales (0 = ninguno)
    float river_width = 1.0f;    // escala de la anchura de los rios
    float river_depth = 1.0f;    // escala de lo que se excavan
    bool lakes = true;
    float lake_min_depth = 2.0f; // metros para que una cuenca sea lago

    // --- Capas ---
    float beach_width = 10.0f;   // metros de playa sobre el mar
    float rock_slope = 42.0f;    // grados desde los que asoma la roca
    float snow_line = 0.72f;     // 0..1 de la altura sobre el mar
    float dry_grass = 0.35f;     // manchas de hierba seca
};

// Rio generado: puntos de la superficie del agua (mundo, mar en y = 0) desde
// el nacimiento hasta la desembocadura, con su anchura.
struct GenRiver {
    std::vector<core::Vec3> points;
    std::vector<float> widths;
};

// Lago: rectangulo de agua (centro y tamano en el mundo) a su nivel.
struct GenLake {
    core::Vec3 center{};  // y = nivel del agua (mundo)
    core::Vec2 size{};
};

struct GenResult {
    TerrainData data;
    std::vector<GenRiver> rivers;
    std::vector<GenLake> lakes;
    core::Vec3 origin{};    // donde va la entidad del terreno
    float erosion_volume = 0.0f;  // m3 que movio la erosion (diagnostico)
    double seconds = 0.0;
};

// Progreso: (0..1, etapa). Devuelve false para cancelar.
using GenProgress = std::function<bool(float progress, const char* stage)>;

// Genera todo. false si se cancelo.
bool generateTerrain(const GenSettings& settings, GenResult& result, const GenProgress& progress = {});

// Las 8 capas del generador (hierba, hierba seca, tierra, roca, arena, grava,
// nieve, barro) con sus colores, repeticion y rugosidad. `texture_folder`
// (dentro de Assets, vacio = solo color): ahi van las texturas generadas.
std::vector<TerrainLayer> generatorLayers(const std::string& texture_folder);
inline constexpr int kGenLayerGrass = 0;
inline constexpr int kGenLayerDryGrass = 1;
inline constexpr int kGenLayerDirt = 2;
inline constexpr int kGenLayerRock = 3;
inline constexpr int kGenLayerSand = 4;
inline constexpr int kGenLayerGravel = 5;
inline constexpr int kGenLayerSnow = 6;
inline constexpr int kGenLayerMud = 7;

// Texturas PBR procedurales y repetibles (color + normal map) de las 8 capas,
// escritas como PNG en `folder` (disco). `size`: pixeles por lado.
// false si no se pudo escribir alguna.
bool writeGeneratorTextures(const std::string& folder, int size = 1024, std::uint32_t seed = 7);
// true si `folder` ya tiene las texturas de esta version del generador.
bool generatorTexturesCurrent(const std::string& folder);

}  // namespace cramion::terrain

#endif  // CRAMION_CORE_TERRAIN_GENERATOR_H
