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
// sus entidades (con su estado actual) en un archivo de la cache de la
// sesion (en disco, no en memoria) y las quita del mundo: no se dibujan, no
// tienen fisica ni scripts. Al volver, el archivo se lee en otro hilo y se
// recrean con el mismo UUID (las referencias entre objetos siguen valiendo),
// unas pocas por frame.
//
// Celdas en disco (mundos grandes, MMO): con "Celdas en disco" activo, al
// guardar la escena el editor escribe esos objetos en <escena>.cells/, un
// archivo por celda, y no en la escena. El juego y el servidor solo leen las
// celdas cercanas: el mundo entero nunca esta en memoria. El editor las
// carga todas al abrir la escena (se ve y se edita entera).
//
// Hilos (CVar wp.AsyncCells, encendida): leer y parsear una celda va al grupo
// de IO y a un hilo trabajador; el hilo principal solo recrea lo ya parseado,
// con un presupuesto de tiempo (wp.RestoreBudgetMs) ademas del de objetos.
// Al descargar, el hilo principal guarda el estado (serializa) y la escritura
// del archivo va al hilo de IO; hasta que termina, lo guardado sigue en
// memoria (volver a cargar la celda lo usa sin leer el disco). Los hilos no
// tocan el sistema ni el mundo (solo su estado compartido).
//
// Origen flotante: las celdas son de la posicion absoluta (origin + local), no
// de la local, asi que no cambian al desplazarse el origen. Lo guardado de una
// celda lleva el origen del mundo de cuando se guardo; al recrearlo, las raices
// se mueven (origen guardado - origen actual) y vuelven a su sitio real.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/jobs/JobSystem.h"

#include <CramionFX/core/Math.h>

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cramion::ecs {
class World;
class Entity;
struct ParsedEntities;
}  // namespace cramion::ecs

namespace cramion::worldpart {

struct WorldPartition {
    bool enabled = true;
    float cell_size = 128.0f;     // metros
    float load_range = 384.0f;    // metros desde cada fuente
    float unload_margin = 64.0f;  // histeresis: se descarga a load_range + esto
    int loads_per_frame = 2;      // celdas que se recrean por frame (sin tirones)
    int unloads_per_frame = 4;    // celdas que se guardan y quitan por frame
    int restores_per_frame = 256; // objetos que se recrean por frame
    bool external_cells = false;  // al guardar, un archivo por celda (<escena>.cells/)
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
    std::size_t disk_bytes = 0;    // lo guardado en disco (cache o celdas externas)
    int loading_cells = 0;         // leyendose o recreandose
    int external_cells = 0;        // celdas de <escena>.cells/ registradas
    int loads = 0;                 // desde el principio
    int unloads = 0;
    int reads_in_flight = 0;       // celdas leyendose/parseandose en otros hilos
    int writes_in_flight = 0;      // celdas escribiendose en otros hilos
};

class WorldPartitionSystem {
public:
    WorldPartitionSystem() = default;
    WorldPartitionSystem(const WorldPartitionSystem&) = delete;
    WorldPartitionSystem& operator=(const WorldPartitionSystem&) = delete;
    ~WorldPartitionSystem();

    // La escena abierta (antes de begin). `cells_in_world`: sus celdas en
    // disco ya estan en el mundo (el editor las carga al abrir); si no (el
    // juego, el servidor), begin() las registra descargadas y las lee al
    // acercarse.
    void setSceneFile(const std::filesystem::path& scene, bool cells_in_world);
    // Carpeta para las celdas descargadas (por defecto, una en el temporal
    // del sistema que se borra al terminar).
    void setCacheFolder(const std::filesystem::path& folder) { cache_root_ = folder; }

    // --- Celdas en disco de una escena (<escena>.cells/cell_X_Z.crcell) ---
    static std::filesystem::path cellsFolder(const std::filesystem::path& scene);
    static bool hasExternalCells(const std::filesystem::path& scene);
    // Editor, al guardar: las raices que se descargan, un archivo por celda.
    // Borra las celdas que ya no tienen nada. Devuelve cuantas celdas; -1 si
    // fallo (con `error`).
    static int writeExternalCells(ecs::World& world, float cell_size, const std::filesystem::path& scene,
                                  std::string* error = nullptr);
    // Borra <escena>.cells/ (se apago "Celdas en disco").
    static void removeExternalCells(const std::filesystem::path& scene);
    // Todas las celdas al mundo (el editor al abrir la escena). Devuelve
    // cuantos objetos raiz.
    static int loadExternalCells(ecs::World& world, const std::filesystem::path& scene);

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
    // Celda de un punto local (del mundo, con el origen flotante actual).
    CellCoord cellOf(const core::Vec3& p) const;
    // Celda de una posicion absoluta (doble precision).
    static CellCoord cellAt(const ecs::DVec3& absolute, float cell_size);
    // El origen del mundo se desplazo (ecs::World::shiftOrigin con el mismo
    // offset). update() tambien lo lee del mundo; esto lo deja bien ya.
    void shiftOrigin(const core::Vec3& offset);
    float cellSize() const { return settings_.cell_size; }
    const PartitionStats& stats() const { return stats_; }
    // Muestra los HLOD de las celdas descargadas y oculta los demas.
    void updateProxies(ecs::World& world) const;
    // Celdas conocidas y si estan cargadas (para dibujar la rejilla).
    std::vector<std::pair<CellCoord, bool>> cells() const;

    // La entidad raiz se descargaria (para el Inspector / pruebas).
    static bool streamable(const ecs::Entity& root);

private:
    // Una raiz por recrear: el texto (camino sincrono) o ya parseada en otro hilo.
    struct PendingRoot {
        std::string text;
        std::shared_ptr<const ecs::ParsedEntities> parsed;
    };
    // Lectura (+ parseo) en otros hilos. Los hilos solo tocan esto.
    struct PendingRead;
    // Escritura en el hilo de IO.
    struct PendingWrite;
    struct Cell {
        bool loaded = true;
        std::vector<entt::entity> roots;  // raices cargadas que viven aqui
        // En memoria: si no se pudo escribir a disco, o mientras se escribe
        // (compartido con el hilo de IO; nadie lo cambia).
        std::shared_ptr<const std::vector<std::string>> stored;
        std::filesystem::path file;       // en disco: cache de la sesion o celda externa
        std::size_t bytes = 0;            // en memoria
        std::size_t disk_bytes = 0;       // en disco
        ecs::DVec3 origin{};              // origen del mundo con el que se guardo
        int entities = 0;  // raices que viven en esta celda
        // Cargando: el archivo se lee (y se parsea) en otro hilo y luego se
        // recrea por trozos.
        bool reading = false;
        std::shared_ptr<PendingRead> read;
        std::shared_ptr<PendingWrite> write;  // escribiendose (stored sigue en memoria)
        // Escrituras anteriores de esta celda que quiza siguen en marcha (la
        // siguiente usa otro nombre mientras quede alguna).
        std::vector<std::shared_ptr<PendingWrite>> older_writes;
        std::vector<PendingRoot> pending;
        std::size_t next = 0;  // siguiente de `pending` por recrear
        bool loading() const { return reading || next < pending.size(); }
    };
    void unloadCell(ecs::World& world, const CellCoord& c, Cell& cell);
    // Empieza a cargar (memoria: ya; disco: en otro hilo). false si no pudo
    // empezar ahora (demasiadas lecturas en marcha: se reintenta).
    bool requestLoad(Cell& cell);
    // Escrituras terminadas: lo guardado sale de la memoria.
    void pollWrites();
    // Cancela lecturas y espera a las escrituras (antes de borrar la cache).
    void drainJobs();
    int readsInFlight() const;
    // Lecturas terminadas y objetos por recrear, con el presupuesto del frame.
    void pumpLoads(ecs::World& world, int budget);
    // Carga entera ya (esperando al disco).
    void loadCellNow(ecs::World& world, Cell& cell);
    void finishLoad(Cell& cell);
    std::filesystem::path cacheFile(const CellCoord& c);
    void registerExternalCells(ecs::World& world);
    void removeCache();
    std::vector<std::pair<core::Vec3, float>> sources(ecs::World& world, const core::Vec3& viewer) const;
    float distanceToCell(const core::Vec3& p, const CellCoord& c) const;
    void track(entt::entity root, const CellCoord& c);
    void untrack(entt::entity root);
    // Raices nuevas (creadas en Play), borradas o que cambiaron de celda. Solo
    // mira las raices (no los hijos) y solo comprueba las nuevas a fondo.
    void rescan(ecs::World& world);
    void refreshStats();

    WorldPartition settings_{};
    ecs::DVec3 origin_{};  // origen flotante del mundo (cellOf con posiciones locales)
    std::map<CellCoord, Cell> cells_;
    std::filesystem::path scene_file_;
    bool cells_in_world_ = true;
    std::filesystem::path cache_root_;  // vacia = temporal del sistema
    std::filesystem::path cache_;       // la de esta sesion (se crea al descargar)
    // Raiz cargada que se puede descargar -> su celda. Con esto cada frame no
    // recorre el mundo entero (antes: todo el arbol y cada hijo, cada frame).
    std::unordered_map<entt::entity, CellCoord> tracked_;
    // Raices ya vistas que no se pueden descargar (no se miran en cada
    // repaso; se olvidan de vez en cuando por si les ponen una malla).
    std::unordered_set<entt::entity> rejected_;
    // Trabajos de disco en marcha (la cache no se borra hasta que acaben).
    std::vector<jobs::JobHandle> io_jobs_;
    std::uint64_t write_seq_ = 0;  // nombre unico por escritura en la cache
    int frame_ = 0;
    int rescans_ = 0;
    bool proxies_dirty_ = true;  // una celda cambio: los HLOD se actualizan
    bool active_ = false;
    PartitionStats stats_{};
};

// El sistema activo del mundo (scripts, MCP, Inspector). nullptr si no hay.
WorldPartitionSystem* activePartition();
void setActivePartition(WorldPartitionSystem* system);

}  // namespace cramion::worldpart

#endif  // CRAMION_CORE_WORLD_WORLD_PARTITION_H
