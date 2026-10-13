#ifndef CRAMION_CORE_TERRAIN_GRID_H
#define CRAMION_CORE_TERRAIN_GRID_H

// Terrenos grandes por teselas (mundos de 10-16 km, MMO):
//
//   TerrainGrid      en una entidad: su posicion es la esquina (x, z minimas)
//                    y la altura 0. Una rejilla de tiles_x * tiles_z teselas
//                    de tile_size metros, cada una en su archivo
//                    <folder>/tile_X_Z.crterrain. Las teselas comparten los
//                    vertices del borde (sin grietas).
//   TerrainStreamer  carga de disco (en otro hilo) solo las teselas cerca de
//                    las fuentes de carga (StreamingSource, o la camara) y
//                    suelta las lejanas: nunca esta el mundo entero en
//                    memoria. Cada tesela cargada es una entidad hija con un
//                    Terrain normal (render, fisica, hierba, navegacion...)
//                    marcada como transitoria: no se guarda en la escena.
//
// Hilos (CVar stream.AsyncTerrain, encendida): la lectura va al grupo de IO,
// el cosido de bordes (con fotos de las vecinas) a un hilo trabajador, y el
// hilo principal solo mete las teselas listas en el mundo con un presupuesto
// de tiempo (stream.MainBudgetMs). Una tesela que sale de la distancia antes
// de terminar se cancela. Al soltar una tesela con cambios se guarda en el
// hilo de IO (TerrainStore::saveAsync).
//
// Importar un mapa de alturas (RAW 16 bits, PNG 16 bits...) a una rejilla:
// HeightmapImport.h.

#include "CramionCore/Uuid.h"
#include "CramionCore/terrain/Terrain.h"

#include <CramionFX/core/Math.h>

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cramion::ecs {
class World;
class Entity;
}  // namespace cramion::ecs

namespace cramion::terrain {

struct TerrainGrid {
    std::string folder = "Terrains/Mundo";  // carpeta en Assets con las teselas
    int tiles_x = 8;
    int tiles_z = 8;
    float tile_size = 1536.0f;     // metros de cada tesela
    float height = 1200.0f;        // altura maxima (el 0..1 de los datos)
    int tile_resolution = 1025;    // vertices por lado de cada tesela (2^n + 1)
    int splat_resolution = 1024;   // texeles de pintura por lado de cada tesela
    float load_range = 2500.0f;    // metros desde cada fuente de carga
    float unload_margin = 400.0f;  // se suelta a load_range + esto (sin parpadeos)
    int loads_per_frame = 1;       // teselas que entran al mundo por frame
    int max_loading = 2;           // lecturas de disco a la vez
    bool stream_in_editor = true;  // en el editor tambien (si no, todas cargadas)
    std::vector<TerrainLayer> layers;
    bool cast_shadows = true;
    bool collision = true;
    float friction = 0.8f;
    float lod_distance = 2.0f;

    TerrainGrid();
    void reflect(ecs::PropertyVisitor& v);

    float worldSizeX() const { return static_cast<float>(tiles_x) * tile_size; }
    float worldSizeZ() const { return static_cast<float>(tiles_z) * tile_size; }
    // Vertices por lado de todo el mundo (los bordes compartidos cuentan una vez).
    std::uint32_t worldResolutionX() const;
    std::uint32_t worldResolutionZ() const;
    // Resolucion de tesela valida (2^n + 1, 33..4097).
    std::uint32_t tileResolution() const;
};

// Marca de una tesela creada por el streaming (no se registra: no se guarda).
struct TerrainTile {
    Uuid grid{};
    int x = 0;
    int z = 0;
};

void registerTerrainGridComponent();

// Ruta (relativa a Assets) del archivo de una tesela.
std::string tilePath(const TerrainGrid& grid, int x, int z);
// El Terrain de una tesela con los ajustes de la rejilla.
void applyGridSettings(const TerrainGrid& grid, Terrain& tile);

// Copia los vertices del borde de la tesela (x, z) a sus vecinas cargadas
// (las 8 de alrededor) donde no coincidan. Devuelve cuantas cambiaron.
int stitchTileBorders(TerrainStore& store, const TerrainGrid& grid, int x, int z);

struct TerrainStreamStats {
    int grids = 0;
    int tiles_total = 0;    // teselas de todas las rejillas
    int tiles_loaded = 0;   // en el mundo ahora
    int tiles_loading = 0;  // leyendose de disco
    std::size_t bytes = 0;  // memoria de las alturas y la pintura cargadas
    int loads = 0;          // desde el principio
    int unloads = 0;
    int integrated = 0;     // teselas que entraron al mundo este frame
    int cancelled = 0;      // cargas canceladas (salieron de la distancia), desde el principio
    int saving = 0;         // guardandose en otro hilo ahora
    double main_ms = 0.0;   // tiempo del hilo principal en update() este frame
};

class TerrainStreamer {
public:
    TerrainStreamer() = default;
    TerrainStreamer(const TerrainStreamer&) = delete;
    TerrainStreamer& operator=(const TerrainStreamer&) = delete;
    ~TerrainStreamer();

    // Cada frame. `viewer`: la camara. `use_sources`: tambien las
    // StreamingSource (en Play y en el juego; en el editor solo la camara).
    // `save_on_unload`: las teselas con cambios se guardan al soltarlas
    // (editor); si no, se quedan en memoria hasta que se guarden.
    void update(ecs::World& world, TerrainStore& store, const core::Vec3& viewer, bool use_sources, bool save_on_unload);
    // Carga ya (esperando a disco) las teselas a `range` metros de `p`: antes
    // de empezar la partida, para que el jugador no caiga al vacio.
    void loadNow(ecs::World& world, TerrainStore& store, const core::Vec3& p, float range = 0.0f);
    // Quita todas las teselas del mundo y suelta sus datos (cerrar la escena).
    // Las cargas en marcha se cancelan y sus resultados no entran nunca. Con
    // save = false lo que tenga cambios sin guardar se queda en el almacen
    // (el que llama decide: TerrainStore::clear lo tira).
    void clear(ecs::World& world, TerrainStore& store, bool save);
    // Generacion de escena (World::sceneGeneration) de lo que hay cargado.
    std::uint64_t sceneGeneration() const { return scene_generation_; }
    const TerrainStreamStats& stats() const { return stats_; }
    // Teselas de una rejilla y si estan cargadas (para dibujar el mapa).
    std::vector<std::pair<std::pair<int, int>, bool>> tiles(const Uuid& grid) const;
    // Sin streaming para las rejillas de esta carpeta (mientras otro hilo
    // reescribe sus teselas: en Windows leerlas a la vez rompe el rename).
    void pauseFolder(const std::string& folder, bool paused);

private:
    // Una carga en otros hilos (lectura + cosido). Los hilos solo tocan esto
    // (por shared_ptr), nunca el streamer ni el almacen.
    struct LoadJob;
    struct Tile {
        entt::entity entity = entt::null;
        std::shared_ptr<LoadJob> job;
        bool loading = false;
        std::uint64_t seen_version = 0;  // para coser los bordes tras esculpir
        std::string path;                // tilePath (sin rehacer la cadena cada frame)
    };
    struct GridState {
        TerrainGrid settings;
        std::map<std::pair<int, int>, Tile> tiles;
        bool alive = false;
    };

    void syncGrid(ecs::World& world, TerrainStore& store, ecs::Entity grid_entity, GridState& state,
                  const std::vector<std::pair<core::Vec3, float>>& sources, bool save_on_unload, int& budget,
                  jobs::FrameBudget& frame_budget, int max_in_flight, bool async);
    void spawn(ecs::World& world, ecs::Entity grid_entity, GridState& state, int x, int z, Tile& tile);
    void unload(ecs::World& world, TerrainStore& store, GridState& state, int x, int z, Tile& tile, bool save,
                bool async = false);
    void startLoad(TerrainStore& store, const GridState& state, int x, int z, Tile& tile, bool async);
    // Una tesela leida (y cosida) entra al almacen; false si no habia nada.
    bool collect(TerrainStore& store, const GridState& state, int x, int z, Tile& tile);
    void cancel(Tile& tile);
    const std::string& pathOf(const GridState& state, int x, int z, Tile& tile) const;
    bool tileAlive(ecs::World& world, const Tile& tile) const;
    int inFlight() const;
    // Otra escena en el mundo (subio World::sceneGeneration): todo lo de la
    // anterior fuera antes de mirar las rejillas (sin guardar: lo que tenga
    // cambios se queda en el almacen).
    void syncSceneGeneration(ecs::World& world, TerrainStore& store);
    // Rejillas que ya no estan (borradas, otra escena): sus teselas fuera
    // ANTES de cargar las vivas (si comparten carpeta, la viva no recibe los
    // datos que la muerta va a soltar o guardar justo despues).
    void pruneDeadGrids(ecs::World& world, TerrainStore& store, bool save, bool async);

    std::map<Uuid, GridState> grids_;
    std::set<std::string> paused_folders_;
    // Cargas canceladas que aun corren en otro hilo (cuentan como en marcha).
    std::vector<std::shared_ptr<LoadJob>> draining_;
    TerrainStreamStats stats_{};
    // World::sceneGeneration con la que se cargo lo que hay (0 = nada aun).
    // Cada LoadJob la apunta: un resultado de otra generacion no entra.
    std::uint64_t scene_generation_ = 0;
};

}  // namespace cramion::terrain

#endif  // CRAMION_CORE_TERRAIN_GRID_H
