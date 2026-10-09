#ifndef CRAMION_CORE_WORLD_WORLD_PARTITION_H
#define CRAMION_CORE_WORLD_WORLD_PARTITION_H

// World Partition (como el de Unreal 5): el mundo se divide en una rejilla de
// celdas y, en Play y en el juego, solo estan cargadas las celdas cerca de las
// fuentes de carga (el jugador, la camara). En el editor la escena se ve y se
// edita entera, como siempre.
//
//   WorldPartition   en una entidad de la escena: tamano de celda, distancia
//                    de carga y cuantas celdas se cargan por frame.
//   StreamingSource  en el jugador (o lo que tenga que cargar el mundo a su
//                    alrededor). Sin ninguna, carga la camara.
//   AlwaysLoaded     esta entidad (y sus hijos) no se descarga nunca.
//
// Se descargan las entidades raiz (sin padre) con mallas que no son
// camaras, terrenos, agua, ambiente, luces del sol, interfaz, jugadores
// (Character Controller, XR) ni objetos de red. Descargar una celda guarda
// sus entidades (con su estado actual) en memoria y las quita del mundo: no
// se dibujan, no tienen fisica ni scripts. Al volver, se recrean con el
// mismo UUID (las referencias entre objetos siguen valiendo).

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cramion::ecs {
class World;
class Entity;
}  // namespace cramion::ecs

namespace cramion::worldpart {

struct WorldPartition {
    bool enabled = true;
    float cell_size = 128.0f;     // metros
    float load_range = 384.0f;    // metros desde cada fuente
    float unload_margin = 64.0f;  // histeresis: se descarga a load_range + esto
    int loads_per_frame = 2;      // celdas que se recrean por frame (sin tirones)
    int unloads_per_frame = 4;    // celdas que se guardan y quitan por frame
    bool show_grid = true;        // rejilla en la vista de escena
    void reflect(ecs::PropertyVisitor& v);
};

struct StreamingSource {
    bool enabled = true;
    float range_scale = 1.0f;  // multiplica la distancia de carga
    void reflect(ecs::PropertyVisitor& v);
};

struct AlwaysLoaded {
    bool enabled = true;
    void reflect(ecs::PropertyVisitor& v);
};

// HLOD: la malla simplificada de una celda (la crea "Construir HLOD" del
// World Partition). Solo se ve en Play cuando su celda esta descargada; en el
// editor, oculta (se ve lo de verdad).
struct HlodProxy {
    int cell_x = 0;
    int cell_z = 0;
    void reflect(ecs::PropertyVisitor& v);
};

void registerWorldPartitionComponents();

struct CellCoord {
    int x = 0;
    int z = 0;
    bool operator<(const CellCoord& o) const { return x != o.x ? x < o.x : z < o.z; }
    bool operator==(const CellCoord& o) const { return x == o.x && z == o.z; }
};

struct PartitionStats {
    bool active = false;
    int cells = 0;
    int loaded_cells = 0;
    int streamed_entities = 0;     // entidades raiz que se pueden descargar
    int unloaded_entities = 0;     // descargadas ahora
    std::size_t stored_bytes = 0;  // lo guardado en memoria de las descargadas
    int loads = 0;                 // desde el principio
    int unloads = 0;
};

class WorldPartitionSystem {
public:
    // Al empezar Play / el juego: reparte las entidades en celdas y descarga
    // las lejanas de `viewer` (y de las StreamingSource).
    void begin(ecs::World& world, const core::Vec3& viewer);
    // Cada frame: carga/descarga segun las fuentes (y `viewer` si no hay).
    void update(ecs::World& world, const core::Vec3& viewer);
    // Al parar: recrea todo lo descargado (el mundo queda como estaba). Con
    // restore = false solo lo olvida (el editor restaura el mundo entero).
    void end(ecs::World& world, bool restore = true);
    bool active() const { return active_; }

    // Carga todo ya (antes de guardar partida, por ejemplo).
    void loadAll(ecs::World& world);
    bool cellLoaded(const CellCoord& c) const;
    CellCoord cellOf(const core::Vec3& p) const;
    float cellSize() const { return settings_.cell_size; }
    const PartitionStats& stats() const { return stats_; }
    // Muestra los HLOD de las celdas descargadas y oculta los demas.
    void updateProxies(ecs::World& world) const;
    // Celdas conocidas y si estan cargadas (para dibujar la rejilla).
    std::vector<std::pair<CellCoord, bool>> cells() const;

    // La entidad raiz se descargaria (para el Inspector / pruebas).
    static bool streamable(const ecs::Entity& root);

private:
    struct Cell {
        bool loaded = true;
        std::vector<entt::entity> roots;  // raices cargadas que viven aqui
        std::vector<std::string> stored;  // entidades guardadas (una raiz por texto)
        std::size_t bytes = 0;
        int entities = 0;  // raices que viven en esta celda
    };
    void unloadCell(ecs::World& world, const CellCoord& c, Cell& cell);
    void loadCell(ecs::World& world, Cell& cell);
    std::vector<std::pair<core::Vec3, float>> sources(ecs::World& world, const core::Vec3& viewer) const;
    float distanceToCell(const core::Vec3& p, const CellCoord& c) const;
    void track(entt::entity root, const CellCoord& c);
    void untrack(entt::entity root);
    // Raices nuevas (creadas en Play), borradas o que cambiaron de celda. Solo
    // mira las raices (no los hijos) y solo comprueba las nuevas a fondo.
    void rescan(ecs::World& world);
    void refreshStats();

    WorldPartition settings_{};
    std::map<CellCoord, Cell> cells_;
    // Raiz cargada que se puede descargar -> su celda. Con esto cada frame no
    // recorre el mundo entero (antes: todo el arbol y cada hijo, cada frame).
    std::unordered_map<entt::entity, CellCoord> tracked_;
    // Raices ya vistas que no se pueden descargar (no se miran en cada
    // repaso; se olvidan de vez en cuando por si les ponen una malla).
    std::unordered_set<entt::entity> rejected_;
    int frame_ = 0;
    int rescans_ = 0;
    bool proxies_dirty_ = true;  // una celda cambio: los HLOD se actualizan
    bool active_ = false;
    PartitionStats stats_{};
};

// El sistema activo del mundo (Lua, MCP, Inspector). nullptr si no hay.
WorldPartitionSystem* activePartition();
void setActivePartition(WorldPartitionSystem* system);

}  // namespace cramion::worldpart

#endif  // CRAMION_CORE_WORLD_WORLD_PARTITION_H
