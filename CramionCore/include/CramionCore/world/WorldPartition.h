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
//   PartitionObject  en un objeto: su rejilla y sus Data Layers.
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
// celdas cercanas: el mundo entero nunca esta en memoria.
//
// Regiones cargadas en el editor (como las Loaded Regions de Unreal 5): con
// "Celdas en disco", el editor tampoco carga la escena entera. Otro
// WorldPartitionSystem en modo editor (beginEditor) tiene cargadas solo las
// celdas a un radio de la camara del editor y las que el usuario fija en la
// ventana "Regiones cargadas"; las carga y descarga con el mismo bombeo con
// presupuesto (y borra lo que descarga por trozos, sin congelar). Lo
// descargado que se edito queda en la cache de la sesion hasta que saveCells
// lo escribe en <escena>.cells/: guardar no pierde nada. Al dar Play, el
// sistema del juego toma las celdas descargadas del editor (inheritUnloadedFrom)
// sin leerlas antes. Parar Play o volver de una pestana rehacen el mundo con
// una instantanea que puede no tener entera una celda que se cargaba:
// recoverAfterRebuild la recupera de donde se cargo. Lo que entra y sale no
// es un paso de deshacer: el editor pone al dia su historial (worldChanges).
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
//
// Fase 1 (como el World Partition de Unreal 5):
//   Rejillas     la del componente es la principal; `grids` anade otras, cada
//                una con su tamano de celda y su distancia de carga (lo grande
//                se carga de lejos, el detalle pequeno solo cerca). Un objeto
//                va a la rejilla que pide su PartitionObject; si no pide
//                ninguna, a la de su etiqueta; si no, a la principal.
//   Data Layers  capas con nombre (`data_layers`). Un objeto esta en las que
//                dice su PartitionObject ("Noche, Ruinas"). En el editor una
//                capa se ve o no (`editor_visible`, sin tocar la escena); en el
//                juego se activa o no (`active` al empezar y luego
//                setDataLayerActive / WorldPartition.setDataLayer): una celda
//                solo se carga si todas sus capas estan activas. Las celdas se
//                separan por capas (como en Unreal): cada celda es (rejilla,
//                capas, x, z).
//   Dependencias `dependencies`: cuando una celda se carga (o se queda), otra
//                tambien (el interior de un edificio con su fachada, un puente
//                con sus dos orillas). En cadena y sin bucles infinitos.
// Todo va en el WorldPartition (y en el PartitionObject de cada objeto), asi
// que se guarda con la escena; las escenas de antes no lo llevan y siguen
// igual (una rejilla, sin capas ni dependencias).

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
#include <set>
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

// Rejilla extra (las Runtime Grids de Unreal). Su indice es el de la lista + 1
// (la 0 es la principal: cell_size / load_range del WorldPartition).
struct PartitionGrid {
    std::string name;            // el que pide el PartitionObject ("Grandes")
    float cell_size = 256.0f;    // metros
    float load_range = 1024.0f;  // metros desde cada fuente
    std::string tag;             // los objetos con esta etiqueta van aqui ("" = solo los que la piden)
    void reflect(ecs::PropertyVisitor& v);
};

// Data Layer: un grupo de objetos que se activa entero.
struct DataLayer {
    std::string name;
    bool active = true;          // cargada al empezar Play / el juego
    bool editor_visible = true;  // se ve en la Escena del editor
    void reflect(ecs::PropertyVisitor& v);
};

// Cuando la celda (grid, x, z) se carga, tambien (load_grid, load_x, load_z).
// Rejillas por nombre ("" = la principal); vale para todas sus capas.
struct CellDependency {
    std::string grid;
    int x = 0;
    int z = 0;
    std::string load_grid;
    int load_x = 0;
    int load_z = 0;
    void reflect(ecs::PropertyVisitor& v);
};

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
    std::vector<PartitionGrid> grids;          // rejillas ademas de la principal
    std::vector<DataLayer> data_layers;        // capas (sin declarar = activa y visible)
    std::vector<CellDependency> dependencies;  // celdas que cargan otras
    void reflect(ecs::PropertyVisitor& v);
};

// En un objeto (la raiz) que se descarga: en que rejilla y en que capas va.
// Con rejilla o capas tambien se descarga aunque no tenga malla (una luz, un
// sonido o un disparador de una capa).
struct PartitionObject {
    std::string grid;         // nombre de la rejilla ("" = la de su etiqueta o la principal)
    std::string data_layers;  // capas separadas por comas ("Noche, Ruinas"; "" = ninguna)
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

// Una celda cualquiera (para dibujar la rejilla y para las pruebas).
struct CellInfo {
    int grid = 0;            // 0 = la principal
    CellCoord coord;
    float cell_size = 0.0f;  // metros (el de su rejilla)
    std::string layers;      // sus capas ("A,B"; "" = ninguna)
    bool loaded = true;
    bool loading = false;
    int entities = 0;     // objetos raiz (cargados o guardados)
    bool pinned = false;  // editor: fijada en "Regiones cargadas"
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
    // Las de otra rejilla o con capas: cell_X_Z_<hash>.crcell, con la rejilla
    // y las capas en la cabecera.
    static std::filesystem::path cellsFolder(const std::filesystem::path& scene);
    static bool hasExternalCells(const std::filesystem::path& scene);
    // Editor, al guardar: las raices que se descargan, un archivo por celda.
    // Borra las celdas que ya no tienen nada. Devuelve cuantas celdas; -1 si
    // fallo (con `error`).
    static int writeExternalCells(ecs::World& world, const WorldPartition& settings, const std::filesystem::path& scene,
                                  std::string* error = nullptr);
    // Solo la rejilla principal, sin capas (como antes).
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

    // Carga todo ya (antes de guardar partida, por ejemplo). Tambien lo de
    // las capas apagadas (hasta el siguiente update).
    void loadAll(ecs::World& world);
    // Celda de la rejilla principal: cargada si alguna de sus capas lo esta
    // (o si no hay nada en ella).
    bool cellLoaded(const CellCoord& c) const;
    bool cellLoaded(int grid, const CellCoord& c) const;
    // Celda de un punto local (del mundo, con el origen flotante actual) en
    // la rejilla principal o en otra.
    CellCoord cellOf(const core::Vec3& p) const;
    CellCoord cellOf(int grid, const core::Vec3& p) const;
    // Celda de una posicion absoluta (doble precision).
    static CellCoord cellAt(const ecs::DVec3& absolute, float cell_size);
    // El origen del mundo se desplazo (ecs::World::shiftOrigin con el mismo
    // offset). update() tambien lo lee del mundo; esto lo deja bien ya.
    void shiftOrigin(const core::Vec3& offset);
    float cellSize() const { return settings_.cell_size; }
    const WorldPartition& settings() const { return settings_; }
    const PartitionStats& stats() const { return stats_; }

    // --- Data Layers en el juego ---
    // Activa o apaga una capa (sus celdas se cargan o se descargan en los
    // siguientes update). Vale antes de begin(): se aplica al empezar. end()
    // lo olvida.
    void setDataLayerActive(const std::string& name, bool active);
    // Una capa sin declarar ni tocar esta activa.
    bool dataLayerActive(const std::string& name) const;
    // Las capas conocidas (las del componente y las tocadas) y su estado.
    std::vector<std::pair<std::string, bool>> dataLayers() const;

    // Muestra los HLOD de las celdas descargadas y oculta los demas.
    void updateProxies(ecs::World& world) const;
    // Celdas conocidas de la rejilla principal y si estan cargadas.
    std::vector<std::pair<CellCoord, bool>> cells() const;
    // Todas las celdas (todas las rejillas y capas).
    std::vector<CellInfo> cellInfos() const;

    // Lo mismo reutilizando `out` (la ventana del editor, cada frame).
    void cellInfos(std::vector<CellInfo>& out) const;

    // La entidad raiz se descargaria (para el Inspector / pruebas).
    static bool streamable(const ecs::Entity& root);

    // --- Regiones cargadas en el editor (Loaded Regions de Unreal 5) ---
    struct EditorRegions {
        bool around_camera = true;  // carga lo que esta a `radius` de la camara
        float radius = 256.0f;      // metros
    };
    // Como begin(), para el editor, con "Celdas en disco" (setSceneFile(escena,
    // false) antes): las celdas en disco quedan registradas sin leer y update()
    // carga y descarga segun la camara y las celdas fijadas, con presupuesto.
    // Sin capas apagadas, dependencias ni HLOD (el editor lo ve todo). Si la
    // escena no tiene un World Partition activo con celdas en disco, no empieza.
    void beginEditor(ecs::World& world, const core::Vec3& camera);
    bool editorMode() const { return editor_mode_; }
    const std::filesystem::path& sceneFile() const { return scene_file_; }
    std::uint64_t sceneGeneration() const { return scene_generation_; }
    void setEditorRegions(const EditorRegions& regions) { regions_ = regions; }
    const EditorRegions& editorRegions() const { return regions_; }
    // Celdas fijadas: cargadas aunque esten lejos (todas sus capas).
    void setCellPinned(int grid, const CellCoord& c, bool pinned);
    bool cellPinned(int grid, const CellCoord& c) const;
    void clearPinnedCells() { pins_.clear(); }
    std::vector<std::pair<int, CellCoord>> pinnedCells() const;
    // Las celdas de estas raices no se descargan (la seleccion del editor).
    void holdCellsOf(ecs::World& world, const std::vector<ecs::Entity>& roots);
    // Misma rejilla (tamanos, nombres y etiquetas): las celdas siguen siendo
    // las mismas. Si no, hay que cargarlo todo (end) y empezar otra vez.
    bool sameLayout(const WorldPartition& settings) const;
    // Lo demas del componente (presupuestos, margen) sin empezar otra vez.
    void refreshSettings(const WorldPartition& settings);
    // La generacion del mundo cambio sin cambiar de escena (parar Play tras un
    // Scene.load, volver de una pestana de prefab): sigue con sus celdas.
    void adoptSceneGeneration(ecs::World& world);
    // El mundo se rehizo entero (parar Play, volver de una pestana, recuperar
    // el autoguardado): los handles ya no valen. El editor lo mira antes de update().
    bool worldRebuilt(const ecs::World& world) const;
    // Reparte el mundo nuevo y recupera de donde se cargaron las celdas que la
    // instantanea no tenia enteras: las que acabaron de cargarse despues de
    // `snapshot_epoch` (loadEpoch() cuando se hizo; 0 = no se sabe: todas).
    void recoverAfterRebuild(ecs::World& world, std::uint64_t snapshot_epoch);
    // Sube cada vez que una celda termina de cargarse en el editor.
    std::uint64_t loadEpoch() const { return load_epoch_; }
    // Termina ya las cargas a medias y los borrados pendientes (antes de Play).
    void settle(ecs::World& world);
    // Guarda <escena>.cells/: las celdas cargadas (del mundo) y las
    // descargadas (de su archivo, de la cache o de la memoria). Fuera del modo
    // editor, igual que writeExternalCells.
    int saveCells(ecs::World& world, const WorldPartition& settings, const std::filesystem::path& scene,
                  std::string* error = nullptr);
    // Play desde el editor con regiones: el siguiente begin() toma las celdas
    // descargadas de `editor` sin tocarlas (lee sus archivos y comparte lo que
    // tiene en memoria, que nadie cambia). nullptr = ninguna.
    void inheritUnloadedFrom(const WorldPartitionSystem* editor) { inherit_from_ = editor; }
    // Raices descargadas que aun se estan borrando por trozos.
    int pendingDestroys() const { return static_cast<int>(dying_.size() - dying_head_); }
    // Sube cada vez que el sistema cambia lo que se guarda del mundo (recrea
    // una raiz o descarga una celda; borrar lo ya descargado no cuenta): el
    // editor pone al dia su historial de deshacer sin paso.
    std::uint64_t worldChanges() const { return world_changes_; }

    // --- Rejillas y capas de un objeto (el editor, el HLOD, las pruebas) ---
    // Rejilla de una raiz: 0 = la principal, i + 1 = settings.grids[i].
    static int gridOf(const WorldPartition& settings, const ecs::Entity& root);
    // Rejilla por nombre ("" = la principal); -1 si no existe.
    static int findGrid(const WorldPartition& settings, const std::string& name);
    static float gridCellSize(const WorldPartition& settings, int grid);
    static float gridLoadRange(const WorldPartition& settings, int grid);
    static std::string gridName(const WorldPartition& settings, int grid);
    // Capas de una raiz ("A,B": ordenadas, sin espacios ni repetidas).
    static std::string layersOf(const ecs::Entity& root);
    static std::string canonicalLayers(const std::string& text);
    static std::vector<std::string> splitLayers(const std::string& canonical);

private:
    // Una celda: rejilla, x, z (en celdas de su rejilla) y capas (indice en
    // layer_sets_; 0 = ninguna). Las de un mismo (rejilla, x, z) van seguidas.
    struct CellKey {
        int grid = 0;
        int x = 0;
        int z = 0;
        int layers = 0;
        bool operator<(const CellKey& o) const {
            if (grid != o.grid) return grid < o.grid;
            if (x != o.x) return x < o.x;
            if (z != o.z) return z < o.z;
            return layers < o.layers;
        }
        bool operator==(const CellKey& o) const { return grid == o.grid && x == o.x && z == o.z && layers == o.layers; }
    };
    // El sitio de una celda sin sus capas (para las dependencias).
    struct GridCoord {
        int grid = 0;
        int x = 0;
        int z = 0;
        bool operator<(const GridCoord& o) const {
            if (grid != o.grid) return grid < o.grid;
            return x != o.x ? x < o.x : z < o.z;
        }
    };
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
        // Editor: de donde se cargo (si el mundo se rehace con una instantanea
        // que no la tenia, vuelve de aqui) y cuando termino de cargarse (loadEpoch).
        std::shared_ptr<const std::vector<std::string>> source_stored;
        std::filesystem::path source_file;
        ecs::DVec3 source_origin{};
        std::uint64_t loaded_epoch = 0;
        // Recuperandose (recoverAfterRebuild): al terminar conserva su epoca.
        bool recovering = false;
        std::uint64_t recover_epoch = 0;
        bool loading() const { return reading || next < pending.size(); }
        bool hasSource() const { return source_stored != nullptr || !source_file.empty(); }
    };
    void start(ecs::World& world, const core::Vec3& viewer, bool editor);
    void unloadCell(ecs::World& world, const CellKey& c, Cell& cell);
    // Editor: lo descargado se desactiva ya (sin UUID, sin guardarse) y se
    // borra de las hojas hacia arriba con wp.EditorDestroyBudgetMs por frame.
    void destroyLater(ecs::World& world, ecs::Entity root);
    void pumpDestroys(ecs::World& world);
    void flushDestroys(ecs::World& world);
    // La celda vuelve a leerse de donde se cargo (con lo que ya hay en el
    // mundo, sin duplicar UUID).
    void reloadFromSource(Cell& cell);
    // Play: las celdas descargadas del editor (inheritUnloadedFrom).
    void inheritCells(ecs::World& world, const WorldPartitionSystem& from);
    // El World Partition de la escena (la entidad) para el modo editor.
    entt::entity findSettingsEntity(ecs::World& world) const;
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
    std::filesystem::path cacheFile(const CellKey& c);
    void registerExternalCells(ecs::World& world);
    void removeCache();
    // Fuentes: posicion y escala de la distancia (la de cada rejilla).
    std::vector<std::pair<core::Vec3, float>> sources(ecs::World& world, const core::Vec3& viewer) const;
    float distanceToCell(const core::Vec3& p, const CellKey& c) const;
    // Lo que se quiere de cada celda este frame: cargarla (dentro de la
    // distancia, o la pide una dependencia) o dejarla (dentro con el margen).
    // `priority`: la distancia que manda (menos = antes; las de capas
    // apagadas, las primeras en salir).
    struct Desire {
        bool load = false;
        bool keep = false;
        float priority = 0.0f;
    };
    std::vector<std::pair<CellKey, Desire>> desires(const std::vector<std::pair<core::Vec3, float>>& srcs) const;
    // Lo mismo en el editor: la camara, las fijadas y la seleccion.
    std::vector<std::pair<CellKey, Desire>> editorDesires(const core::Vec3& camera) const;
    // Celda de una raiz (rejilla, capas y sitio) con el origen de ahora.
    CellKey keyOf(const ecs::Entity& root);
    int layerSetId(const std::string& canonical);
    int layerIdOf(const ecs::Entity& root);
    bool layerSetActive(int id) const;
    // Recrea una raiz de un archivo de celda y la reparte en su celda.
    void restoreTracked(ecs::World& world, const std::string& text, const ecs::DVec3& saved);
    void track(entt::entity root, const CellKey& c);
    void untrack(entt::entity root);
    // Raices nuevas (creadas en Play), borradas o que cambiaron de celda. Solo
    // mira las raices (no los hijos) y solo comprueba las nuevas a fondo.
    void rescan(ecs::World& world);
    void refreshStats();

    WorldPartition settings_{};
    ecs::DVec3 origin_{};  // origen flotante del mundo (cellOf con posiciones locales)
    std::map<CellKey, Cell> cells_;
    // Conjuntos de capas ("" = 0, "A,B"...) y sus nombres sueltos.
    std::vector<std::string> layer_sets_{std::string()};
    std::vector<std::vector<std::string>> layer_names_{std::vector<std::string>()};
    std::unordered_map<std::string, int> layer_set_ids_{{std::string(), 0}};
    // Texto del PartitionObject -> conjunto (sin rehacerlo en cada repaso).
    std::unordered_map<std::string, int> raw_layer_ids_;
    std::map<std::string, bool> layer_state_;      // capas activas en el juego
    std::map<std::string, bool> layer_overrides_;  // las tocadas (sobreviven a begin)
    std::map<GridCoord, std::vector<GridCoord>> deps_;
    std::filesystem::path scene_file_;
    bool cells_in_world_ = true;
    std::filesystem::path cache_root_;  // vacia = temporal del sistema
    std::filesystem::path cache_;       // la de esta sesion (se crea al descargar)
    // Raiz cargada que se puede descargar -> su celda. Con esto cada frame no
    // recorre el mundo entero (antes: todo el arbol y cada hijo, cada frame).
    std::unordered_map<entt::entity, CellKey> tracked_;
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
    // World::sceneGeneration con la que se hizo begin(). Si el mundo cambia de
    // escena sin end(), update() lo olvida todo (sus celdas y sus handles son
    // de la escena anterior: nada de ella vuelve a entrar en la nueva).
    std::uint64_t scene_generation_ = 0;
    PartitionStats stats_{};
    // --- Modo editor (regiones cargadas) ---
    bool editor_mode_ = false;
    EditorRegions regions_{};
    std::set<GridCoord> pins_;     // fijadas a mano
    std::vector<GridCoord> held_;  // de la seleccion (ordenadas)
    // Raices descargadas por borrar (de dying_head_ en adelante).
    std::vector<entt::entity> dying_;
    std::size_t dying_head_ = 0;
    // La entidad con el WorldPartition: si cambia, el mundo se rehizo.
    entt::entity settings_entity_ = entt::null;
    std::uint64_t load_epoch_ = 0;
    std::uint64_t world_changes_ = 0;
    const WorldPartitionSystem* inherit_from_ = nullptr;
};

// El sistema activo del mundo (scripts, MCP, Inspector). nullptr si no hay.
WorldPartitionSystem* activePartition();
void setActivePartition(WorldPartitionSystem* system);

}  // namespace cramion::worldpart

#endif  // CRAMION_CORE_WORLD_WORLD_PARTITION_H
