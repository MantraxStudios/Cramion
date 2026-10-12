#include "CramionCore/terrain/TerrainGrid.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/foliage/Foliage.h"
#include "CramionCore/world/WorldPartition.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/jobs/JobSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>

namespace cramion::terrain {

using core::Vec3;
using ecs::FloatRange;

namespace {

std::uint32_t validResolution(int value) {
    const auto res = static_cast<std::uint32_t>(std::clamp(value, 33, 4097));
    std::uint32_t pow = 32;
    while (pow + 1 < res) pow *= 2;
    return pow + 1;
}

// UUID fijo por tesela (rejilla + coordenadas): la seleccion y el deshacer
// del editor la siguen encontrando cuando vuelve a cargarse.
Uuid tileUuid(const Uuid& grid, int x, int z) {
    std::uint64_t h = 0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) ^
                      static_cast<std::uint32_t>(z);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    Uuid id{grid.high ^ h, grid.low ^ (h * 0x2545F4914F6CDD1Dull)};
    id.high = (id.high & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    id.low = (id.low & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
    return id;
}

// Distancia en el suelo (XZ) de un punto al cuadrado de una tesela.
float distanceToTile(const Vec3& p, const Vec3& corner, float size, int x, int z) {
    const float x0 = corner.x + static_cast<float>(x) * size;
    const float z0 = corner.z + static_cast<float>(z) * size;
    const float dx = std::max({x0 - p.x, 0.0f, p.x - (x0 + size)});
    const float dz = std::max({z0 - p.z, 0.0f, p.z - (z0 + size)});
    return std::sqrt(dx * dx + dz * dz);
}

bool sameLayout(const TerrainGrid& a, const TerrainGrid& b) {
    return a.folder == b.folder && a.tiles_x == b.tiles_x && a.tiles_z == b.tiles_z && a.tile_size == b.tile_size &&
           a.tileResolution() == b.tileResolution() && a.splat_resolution == b.splat_resolution;
}

std::size_t dataBytes(const TerrainData& d) {
    return d.heights().size() * sizeof(float) + d.splat0().size() + d.splat1().size();
}

}  // namespace

// -----------------------------------------------------------------------------
// Componente
// -----------------------------------------------------------------------------

TerrainGrid::TerrainGrid() { layers = Terrain{}.layers; }

std::uint32_t TerrainGrid::tileResolution() const { return validResolution(tile_resolution); }

std::uint32_t TerrainGrid::worldResolutionX() const {
    return static_cast<std::uint32_t>(std::max(tiles_x, 1)) * (tileResolution() - 1) + 1;
}

std::uint32_t TerrainGrid::worldResolutionZ() const {
    return static_cast<std::uint32_t>(std::max(tiles_z, 1)) * (tileResolution() - 1) + 1;
}

void TerrainGrid::reflect(ecs::PropertyVisitor& v) {
    v.field({"folder", "Carpeta", "Carpeta de Assets con las teselas (tile_X_Z.crterrain)"}, folder);
    v.field({"tiles_x", "Teselas en X"}, tiles_x, 1, 256);
    v.field({"tiles_z", "Teselas en Z"}, tiles_z, 1, 256);
    v.field({"tile_size", "Tamano de tesela", "Metros de cada tesela en X y en Z"}, tile_size,
            FloatRange{16.0f, 16384.0f, 1.0f, "%.0f m"});
    v.field({"height", "Altura maxima"}, height, FloatRange{0.1f, 8192.0f, 0.5f, "%.1f m"});
    if (v.wantsAllFields() || v.beginGroup("Resolucion de cada tesela", false)) {
        v.field({"tile_resolution", "Alturas", "Vertices por lado de cada tesela (257, 513, 1025, 2049)"}, tile_resolution,
                33, 4097);
        v.field({"splat_resolution", "Pintura", "Texeles de los pesos de las capas por lado de cada tesela"},
                splat_resolution, 16, 4096);
        if (!v.wantsAllFields()) v.endGroup();
    }
    if (v.wantsAllFields() || v.beginGroup("Carga por zonas (streaming)", true)) {
        v.field({"load_range", "Distancia de carga", "Teselas a esta distancia de una fuente de carga (o la camara)"},
                load_range, FloatRange{64.0f, 50000.0f, 10.0f, "%.0f m"});
        v.field({"unload_margin", "Margen de descarga", "Se sueltan a la distancia de carga mas esto"}, unload_margin,
                FloatRange{0.0f, 10000.0f, 10.0f, "%.0f m"});
        v.field({"loads_per_frame", "Teselas por frame", "Teselas que entran al mundo cada frame (menos = sin tirones)"},
                loads_per_frame, 1, 16);
        v.field({"max_loading", "Lecturas a la vez", "Teselas que se leen de disco a la vez en otros hilos"}, max_loading,
                1, 8);
        v.field({"stream_in_editor", "Tambien en el editor",
                 "En el editor solo se cargan las teselas cerca de la camara (si no, todas: cuidado con la memoria)"},
                stream_in_editor);
        if (!v.wantsAllFields()) v.endGroup();
    }
    v.field({"cast_shadows", "Proyecta sombras"}, cast_shadows);
    v.field({"collision", "Colision (fisica)"}, collision);
    v.field({"friction", "Friccion"}, friction, FloatRange{0.0f, 2.0f, 0.01f, "%.2f", true});
    v.field({"lod_distance", "Detalle a distancia", "Mas = mas triangulos lejos"}, lod_distance,
            FloatRange{0.5f, 8.0f, 0.05f, "%.2f", true});
    reflectTerrainLayers(v, layers);
}

void registerTerrainGridComponent() {
    auto& r = ecs::ComponentRegistry::instance();
    if (r.find("TerrainGrid") != nullptr) return;
    r.registerComponent<TerrainGrid>("TerrainGrid", "Terreno grande (teselas)", "Entorno");
}

std::string tilePath(const TerrainGrid& grid, int x, int z) {
    std::string folder = grid.folder;
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();
    return folder + "/tile_" + std::to_string(x) + "_" + std::to_string(z) + ".crterrain";
}

void applyGridSettings(const TerrainGrid& grid, Terrain& tile) {
    tile.size = std::max(grid.tile_size, 1.0f);
    tile.height = grid.height;
    tile.resolution = static_cast<int>(grid.tileResolution());
    tile.splat_resolution = grid.splat_resolution;
    tile.layers = grid.layers;
    tile.cast_shadows = grid.cast_shadows;
    tile.collision = grid.collision;
    tile.friction = grid.friction;
    tile.lod_distance = grid.lod_distance;
}

// -----------------------------------------------------------------------------
// Coser bordes
// -----------------------------------------------------------------------------

namespace {

// Copia a `dst` los vertices que comparte con `src` (alturas `src_heights`
// de `n` x `n`), siendo `dst` la vecina (dx, dz) de `src`. true si cambio algo.
bool copySharedBorderFrom(const std::vector<float>& src_heights, std::uint32_t src_res, TerrainData& dst, int dx,
                          int dz) {
    const int n = static_cast<int>(src_res);
    if (static_cast<int>(dst.resolution()) != n || src_heights.size() != static_cast<std::size_t>(n) * n) return false;
    const int last = n - 1;
    // Vertices compartidos: la columna/fila del borde comun (o la esquina).
    const int sx0 = dx < 0 ? 0 : dx > 0 ? last : 0, sx1 = dx < 0 ? 0 : dx > 0 ? last : last;
    const int sz0 = dz < 0 ? 0 : dz > 0 ? last : 0, sz1 = dz < 0 ? 0 : dz > 0 ? last : last;
    // Primero mirar sin escribir: heights() no const copiaria el buffer si
    // alguien tiene una foto, aunque luego no cambiara nada.
    const std::vector<float>& current = static_cast<const TerrainData&>(dst).heights();
    bool differs = false;
    for (int sz = sz0; sz <= sz1 && !differs; ++sz) {
        for (int sx = sx0; sx <= sx1; ++sx) {
            const int tx = sx - dx * last, tz = sz - dz * last;
            if (current[static_cast<std::size_t>(tz) * n + tx] != src_heights[static_cast<std::size_t>(sz) * n + sx]) {
                differs = true;
                break;
            }
        }
    }
    if (!differs) return false;
    std::vector<float>& heights = dst.heights();
    int x0 = n, z0 = n, x1 = -1, z1 = -1;
    for (int sz = sz0; sz <= sz1; ++sz) {
        for (int sx = sx0; sx <= sx1; ++sx) {
            const int tx = sx - dx * last, tz = sz - dz * last;
            const float value = src_heights[static_cast<std::size_t>(sz) * n + sx];
            float& target = heights[static_cast<std::size_t>(tz) * n + tx];
            if (target == value) continue;
            target = value;
            x0 = std::min(x0, tx);
            z0 = std::min(z0, tz);
            x1 = std::max(x1, tx);
            z1 = std::max(z1, tz);
        }
    }
    if (x1 < 0) return false;
    dst.markHeights(x0, z0, x1, z1);
    dst.commitCollision();
    return true;
}

bool copySharedBorder(const TerrainData& src, TerrainData& dst, int dx, int dz) {
    return copySharedBorderFrom(src.heights(), src.resolution(), dst, dx, dz);
}

// Al revés que stitchTileBorders: la tesela (x, z), recien cargada, toma los
// bordes de sus vecinas ya en memoria (pueden tener cambios que no estaban
// en su archivo). Devuelve cuantas vecinas aportaron algo.
int stitchFromNeighbours(TerrainStore& store, const TerrainGrid& grid, int x, int z) {
    const std::shared_ptr<TerrainData> dst = store.findPath(tilePath(grid, x, z));
    if (!dst) return 0;
    int changed = 0;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const int nx = x + dx, nz = z + dz;
            if (nx < 0 || nz < 0 || nx >= grid.tiles_x || nz >= grid.tiles_z) continue;
            const std::shared_ptr<TerrainData> src = store.findPath(tilePath(grid, nx, nz));
            // La vecina esta en (dx, dz) de esta: esta es la (-dx, -dz) de la vecina.
            if (src && copySharedBorder(*src, *dst, -dx, -dz)) ++changed;
        }
    }
    return changed;
}

int neighbourIndex(int dx, int dz) { return (dz + 1) * 3 + (dx + 1); }

}  // namespace

int stitchTileBorders(TerrainStore& store, const TerrainGrid& grid, int x, int z) {
    const std::shared_ptr<TerrainData> src = store.findPath(tilePath(grid, x, z));
    if (!src) return 0;
    int changed = 0;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const int nx = x + dx, nz = z + dz;
            if (nx < 0 || nz < 0 || nx >= grid.tiles_x || nz >= grid.tiles_z) continue;
            const std::shared_ptr<TerrainData> dst = store.findPath(tilePath(grid, nx, nz));
            if (dst && copySharedBorder(*src, *dst, dx, dz)) ++changed;
        }
    }
    return changed;
}

// -----------------------------------------------------------------------------
// Streaming
// -----------------------------------------------------------------------------

namespace {

cvar::CVar<bool> g_async_terrain("stream.AsyncTerrain", true,
                                 "Teselas de terreno: leer y coser en otros hilos, entrar al mundo con presupuesto de "
                                 "tiempo y guardar al soltar en el hilo de IO (apagado: como antes)",
                                 cvar::Saved);
cvar::CVar<float> g_main_budget_ms("stream.MainBudgetMs", 2.0f,
                                   "Milisegundos por frame del hilo principal para meter en el mundo lo que se cargo en "
                                   "otros hilos (siempre entra al menos una cosa)",
                                   cvar::Saved, 0.0f, 100.0f);
cvar::CVar<int> g_max_in_flight("stream.MaxInFlight", -1,
                                "Teselas cargandose a la vez en otros hilos (-1 = automatico: 4, o 2 en equipos de 4 "
                                "hilos o menos)",
                                cvar::Saved, -1, 32);

bool lowTier() { return jobs::workerCount() <= 4; }

double mainBudgetMs() {
    const double ms = g_main_budget_ms.get();
    // Por defecto, menos en equipos pequenos.
    if (g_main_budget_ms.isDefault() && lowTier()) return 1.0;
    return ms;
}

int maxInFlight(const TerrainGrid& g) {
    const int cvar_value = g_max_in_flight.get();
    if (cvar_value >= 1) return cvar_value;
    // max_loading distinto del de siempre (2): lo pidio la rejilla.
    if (g.max_loading != 2) return std::max(g.max_loading, 1);
    return lowTier() ? 2 : 4;
}

// Foto de una vecina tomada en el hilo principal al lanzar la carga.
struct NeighbourShot {
    int dx = 0, dz = 0;
    std::shared_ptr<const TerrainSnapshot> shot;
};

}  // namespace

struct TerrainStreamer::LoadJob {
    jobs::Cancel cancel = std::make_shared<jobs::CancelToken>();
    std::atomic<bool> done{false};
    // Lo escribe el hilo antes de done = true; el principal lo lee despues.
    std::shared_ptr<TerrainData> data;
    // Versiones de las vecinas con las que se cosio (0 = no estaba).
    std::array<std::uint64_t, 9> neighbour_version{};
    bool async = false;  // cosida en el hilo (si no, se cose al entrar, como antes)
    bool ready() const { return done.load(std::memory_order_acquire); }
};

TerrainStreamer::~TerrainStreamer() {
    // Los hilos solo tienen su LoadJob (shared_ptr): se cancelan y acaban
    // solos, sin esperar ni tocar el streamer.
    for (auto& [uuid, state] : grids_) {
        for (auto& [key, tile] : state.tiles) cancel(tile);
    }
}

bool TerrainStreamer::tileAlive(ecs::World& world, const Tile& tile) const {
    return tile.entity != entt::null && world.valid(tile.entity) && world.registry().all_of<TerrainTile>(tile.entity);
}

const std::string& TerrainStreamer::pathOf(const GridState& state, int x, int z, Tile& tile) const {
    if (tile.path.empty()) tile.path = tilePath(state.settings, x, z);
    return tile.path;
}

int TerrainStreamer::inFlight() const {
    int n = 0;
    for (const auto& [uuid, state] : grids_) {
        for (const auto& [key, tile] : state.tiles) n += tile.loading ? 1 : 0;
    }
    for (const auto& job : draining_) n += job->ready() ? 0 : 1;
    return n;
}

void TerrainStreamer::cancel(Tile& tile) {
    if (tile.job) {
        tile.job->cancel->cancelled.store(true, std::memory_order_release);
        if (!tile.job->ready()) draining_.push_back(tile.job);
        tile.job.reset();
        ++stats_.cancelled;
    }
    tile.loading = false;
}

void TerrainStreamer::startLoad(TerrainStore& store, const GridState& state, int x, int z, Tile& tile, bool async) {
    const std::string& path = pathOf(state, x, z, tile);
    // Se estaba guardando (se solto hace poco): vuelve de la memoria, sin leer
    // un archivo a medio escribir.
    if (std::shared_ptr<TerrainData> saved = store.takeSaving(path)) {
        store.put(path, std::move(saved));
        tile.loading = false;
        return;
    }
    const std::filesystem::path file = store.absolute(path);
    const std::uint32_t res = state.settings.tileResolution();
    const auto splat = static_cast<std::uint32_t>(std::clamp(state.settings.splat_resolution, 16, 4096));
    auto job = std::make_shared<LoadJob>();
    job->async = async;
    // Fotos de las vecinas en memoria (aqui, en el hilo principal): el hilo
    // trabajador cose con ellas sin tocar los datos vivos.
    std::vector<NeighbourShot> shots;
    if (async) {
        const TerrainGrid& g = state.settings;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) continue;
                const int nx = x + dx, nz = z + dz;
                if (nx < 0 || nz < 0 || nx >= g.tiles_x || nz >= g.tiles_z) continue;
                std::shared_ptr<TerrainData> nd;
                if (const auto it = state.tiles.find({nx, nz}); it != state.tiles.end() && !it->second.path.empty()) {
                    nd = store.findPath(it->second.path);
                } else {
                    nd = store.findPath(tilePath(g, nx, nz));
                }
                if (!nd) continue;
                shots.push_back(NeighbourShot{dx, dz, nd->snapshot()});
                job->neighbour_version[static_cast<std::size_t>(neighbourIndex(dx, dz))] = nd->version();
            }
        }
    }
    tile.loading = true;
    tile.job = job;
    // Sin `this` ni el almacen: solo el LoadJob, la ruta y las fotos.
    jobs::scheduleIo(
        [job, file, res, splat, async, shots = std::move(shots)]() mutable {
            if (jobs::isCancelled(job->cancel)) {
                job->done.store(true, std::memory_order_release);
                return;
            }
            auto data = std::make_shared<TerrainData>();
            std::error_code ec;
            if (!std::filesystem::exists(file, ec) || !data->load(file)) {
                data->create(res, splat);  // tesela nueva: plana (se guarda al editarla)
                data->setSaved();
            }
            if (!async || shots.empty() || jobs::isCancelled(job->cancel)) {
                job->data = std::move(data);
                job->done.store(true, std::memory_order_release);
                return;
            }
            // Coser los bordes en un hilo de calculo (el de IO sigue leyendo).
            jobs::schedule(
                [job, data = std::move(data), shots = std::move(shots)]() mutable {
                    if (!jobs::isCancelled(job->cancel)) {
                        // Datos aun sin compartir: este hilo es el unico que los tiene.
                        for (const NeighbourShot& n : shots) {
                            if (!n.shot || !n.shot->heights) continue;
                            // La vecina esta en (dx, dz) de esta: esta es la (-dx, -dz) de la vecina.
                            copySharedBorderFrom(*n.shot->heights, n.shot->resolution, *data, -n.dx, -n.dz);
                        }
                        shots.clear();  // suelta las fotos ya (copia al escribir en el principal)
                        job->data = std::move(data);
                    }
                    job->done.store(true, std::memory_order_release);
                },
                nullptr, jobs::Priority::Normal);
        },
        jobs::Priority::Normal);
}

bool TerrainStreamer::collect(TerrainStore& store, const GridState& state, int x, int z, Tile& tile) {
    if (!tile.loading || !tile.job || !tile.job->ready()) return false;
    std::shared_ptr<LoadJob> job = std::move(tile.job);
    tile.job.reset();
    tile.loading = false;
    if (!job->data) return false;
    // Si mientras tanto ya hay datos en el almacen (un deshacer del editor
    // los devolvio), mandan esos: el archivo es mas viejo.
    const std::string& path = pathOf(state, x, z, tile);
    if (store.findPath(path)) return false;
    store.put(path, std::move(job->data));
    if (!job->async) return true;
    // Vecinas que cambiaron (o llegaron) despues de la foto: se cose aqui,
    // solo con esas (una fila de vertices, barato).
    const std::shared_ptr<TerrainData> dst = store.findPath(path);
    const TerrainGrid& g = state.settings;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const int nx = x + dx, nz = z + dz;
            if (nx < 0 || nz < 0 || nx >= g.tiles_x || nz >= g.tiles_z) continue;
            std::shared_ptr<TerrainData> src;
            if (const auto it = state.tiles.find({nx, nz}); it != state.tiles.end() && !it->second.path.empty()) {
                src = store.findPath(it->second.path);
            } else {
                src = store.findPath(tilePath(g, nx, nz));
            }
            if (!src) continue;
            if (job->neighbour_version[static_cast<std::size_t>(neighbourIndex(dx, dz))] == src->version()) continue;
            copySharedBorder(*src, *dst, -dx, -dz);
        }
    }
    return true;
}

void TerrainStreamer::spawn(ecs::World& world, ecs::Entity grid_entity, GridState& state, int x, int z, Tile& tile) {
    const Uuid uuid = tileUuid(grid_entity.uuid(), x, z);
    // Un resto de antes (el mundo se restauro con la tesela dentro): fuera.
    if (ecs::Entity old = world.find(uuid); old.valid()) world.destroy(old);
    // Raiz transitoria (no hija de la rejilla): deshacer o parar Play rehacen
    // la escena sin tocarla (ver World::clearPersistent) y no se guarda.
    ecs::Entity e = world.create("Tesela " + std::to_string(x) + "," + std::to_string(z), {}, uuid);
    e.get<ecs::EntityInfo>().transient = true;
    e.setLocalPosition(grid_entity.worldPosition() + Vec3{static_cast<float>(x) * state.settings.tile_size, 0.0f,
                                                          static_cast<float>(z) * state.settings.tile_size});
    Terrain& t = e.add<Terrain>();
    t.data = pathOf(state, x, z, tile);
    applyGridSettings(state.settings, t);
    if (const foliage::Grass* grass = grid_entity.tryGet<foliage::Grass>()) e.add<foliage::Grass>(*grass);
    e.add<TerrainTile>(TerrainTile{grid_entity.uuid(), x, z});
    tile.entity = e.handle();
    ++stats_.loads;
}

void TerrainStreamer::unload(ecs::World& world, TerrainStore& store, GridState& state, int x, int z, Tile& tile,
                             bool save, bool async) {
    cancel(tile);
    if (tileAlive(world, tile)) world.destroy(world.wrap(tile.entity));
    tile.entity = entt::null;
    const std::string path = pathOf(state, x, z, tile);
    if (const std::shared_ptr<TerrainData> data = store.findPath(path)) {
        if (data->unsaved()) {
            // Con cambios: se guardan (editor) o se quedan en memoria hasta
            // que se guarde la escena (Play: no se pierde lo esculpido).
            if (!save) return;
            if (async) {
                // En el hilo de IO con una foto (la entidad ya no esta: nadie
                // los cambia). Sale del almacen; volver a cargarla mientras se
                // escribe reutiliza estos datos (takeSaving).
                store.saveAsync(path, data);
                ++stats_.unloads;
                return;
            }
            // Una escritura en otro hilo de esta ruta: que termine antes.
            if (store.pendingSave(path)) store.flushSaves();
            if (!data->save(store.absolute(path))) {
                std::cerr << "[Terreno] No se pudo guardar la tesela " << path << "\n";
                return;
            }
        }
        store.release(path);
    }
    ++stats_.unloads;
}

void TerrainStreamer::syncGrid(ecs::World& world, TerrainStore& store, ecs::Entity grid_entity, GridState& state,
                               const std::vector<std::pair<Vec3, float>>& sources, bool save_on_unload, int& budget,
                               jobs::FrameBudget& frame_budget, int max_in_flight, bool async) {
    const TerrainGrid& g = state.settings;
    const Vec3 corner = grid_entity.worldPosition();
    const float size = std::max(g.tile_size, 1.0f);
    const bool everything = sources.empty();  // sin streaming: todas
    // Lecturas terminadas: los datos al almacen (la entidad entra con el presupuesto).
    for (auto& [key, tile] : state.tiles) collect(store, state, key.first, key.second, tile);
    int reading = inFlight();

    // Lo que debe estar (mas cerca primero) y lo que sobra.
    struct Want {
        float distance;
        int x, z;
    };
    std::vector<Want> wanted;
    for (int z = 0; z < g.tiles_z; ++z) {
        for (int x = 0; x < g.tiles_x; ++x) {
            float best = 1e30f;
            for (const auto& [p, range] : sources) best = std::min(best, distanceToTile(p, corner, size, x, z) - range);
            if (everything) best = -1.0f;
            auto it = state.tiles.find({x, z});
            const bool present = it != state.tiles.end();
            if (best <= 0.0f) {
                wanted.push_back({best, x, z});
            } else if (present && best > g.unload_margin && (async || !it->second.loading)) {
                // Cargandose aun: se cancela (el hilo descarta lo leido; hasta
                // que acabe cuenta como en marcha).
                unload(world, store, state, x, z, it->second, save_on_unload, async);
                state.tiles.erase(it);
            }
        }
    }
    std::sort(wanted.begin(), wanted.end(), [](const Want& a, const Want& b) { return a.distance < b.distance; });
    for (const Want& w : wanted) {
        Tile& tile = state.tiles[{w.x, w.z}];
        if (tileAlive(world, tile) || tile.loading) continue;
        const std::string& path = pathOf(state, w.x, w.z, tile);
        if (store.findPath(path)) {
            if (budget <= 0) continue;
            if (async && !frame_budget.spend()) continue;
            spawn(world, grid_entity, state, w.x, w.z, tile);
            // Sus bordes, los de las vecinas ya cargadas (sin grietas si se
            // esculpieron mientras esta no estaba). Con la carga en otros
            // hilos ya vienen cosidos (collect cose solo lo que cambio).
            if (!async) stitchFromNeighbours(store, g, w.x, w.z);
            if (const auto data = store.findPath(path)) tile.seen_version = data->version();
            --budget;
            ++stats_.integrated;
        } else if (reading < max_in_flight) {
            startLoad(store, state, w.x, w.z, tile, async);
            if (tile.loading) ++reading;
        }
    }
    // Tras esculpir una tesela (el trazo ya termino: colision al dia), sus
    // bordes pasan a las vecinas.
    for (auto& [key, tile] : state.tiles) {
        if (!tileAlive(world, tile)) continue;
        const std::shared_ptr<TerrainData> data = store.findPath(pathOf(state, key.first, key.second, tile));
        if (!data || data->version() == tile.seen_version || data->collisionVersion() != data->version()) continue;
        stitchTileBorders(store, g, key.first, key.second);
        tile.seen_version = data->version();
        // Las vecinas cambiadas no tienen que coser de vuelta.
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                auto n = state.tiles.find({key.first + dx, key.second + dz});
                if (n == state.tiles.end()) continue;
                if (const auto nd = store.findPath(pathOf(state, n->first.first, n->first.second, n->second))) {
                    n->second.seen_version = nd->version();
                }
            }
        }
    }
    // Ajustes de la rejilla (capas, sombras...) y su posicion en las teselas
    // cargadas (se mueve la rejilla: se mueven todas).
    for (auto& [key, tile] : state.tiles) {
        if (!tileAlive(world, tile)) continue;
        ecs::Entity te = world.wrap(tile.entity);
        if (Terrain* t = te.tryGet<Terrain>()) applyGridSettings(g, *t);
        const Vec3 want = corner + Vec3{static_cast<float>(key.first) * size, 0.0f, static_cast<float>(key.second) * size};
        const Vec3 now = te.localPosition();
        if (now.x != want.x || now.y != want.y || now.z != want.z) te.setLocalPosition(want);
        // Hierba: la de la rejilla.
        if (const foliage::Grass* grass = grid_entity.tryGet<foliage::Grass>()) {
            if (foliage::Grass* own = te.tryGet<foliage::Grass>()) {
                *own = *grass;
            } else {
                te.add<foliage::Grass>(*grass);
            }
        } else if (te.has<foliage::Grass>()) {
            te.remove<foliage::Grass>();
        }
    }
}

void TerrainStreamer::update(ecs::World& world, TerrainStore& store, const Vec3& viewer, bool use_sources,
                             bool save_on_unload) {
    const auto start = std::chrono::steady_clock::now();
    const bool async = g_async_terrain.get();
    // Guardados en otro hilo que terminaron y cargas canceladas que acabaron.
    store.pollSaves();
    draining_.erase(std::remove_if(draining_.begin(), draining_.end(),
                                   [](const std::shared_ptr<LoadJob>& j) { return j->ready(); }),
                    draining_.end());
    jobs::FrameBudget frame_budget(mainBudgetMs());
    stats_.integrated = 0;
    for (auto& [uuid, state] : grids_) state.alive = false;
    stats_.grids = 0;
    stats_.tiles_total = 0;
    for (const entt::entity h : world.registry().view<TerrainGrid>()) {
        ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const TerrainGrid& g = e.get<TerrainGrid>();
        if (paused_folders_.count(g.folder) != 0) {
            // Pausada (se reescriben sus archivos): ni lee ni guarda nada; lo
            // que hubiera cargado se queda como esta.
            if (const auto it = grids_.find(e.uuid()); it != grids_.end()) it->second.alive = true;
            ++stats_.grids;
            stats_.tiles_total += std::max(g.tiles_x, 0) * std::max(g.tiles_z, 0);
            continue;
        }
        GridState& state = grids_[e.uuid()];
        if (!sameLayout(state.settings, g) && !state.tiles.empty()) {
            // Otra carpeta o tamano: lo cargado ya no vale (lo que se leia se
            // cancela; sus rutas cacheadas son de la carpeta de antes).
            for (auto& [key, tile] : state.tiles) unload(world, store, state, key.first, key.second, tile, save_on_unload, async);
            state.tiles.clear();
        }
        state.settings = g;
        state.alive = true;
        ++stats_.grids;
        stats_.tiles_total += std::max(g.tiles_x, 0) * std::max(g.tiles_z, 0);
        std::vector<std::pair<Vec3, float>> sources;
        if (use_sources || g.stream_in_editor) {
            if (use_sources) {
                for (const entt::entity sh : world.registry().view<worldpart::StreamingSource>()) {
                    const ecs::Entity se = world.wrap(sh);
                    const worldpart::StreamingSource& s = se.get<worldpart::StreamingSource>();
                    if (!s.enabled || !se.activeInHierarchy()) continue;
                    sources.emplace_back(se.worldPosition(), g.load_range * std::max(s.range_scale, 0.01f));
                }
            }
            if (sources.empty()) sources.emplace_back(viewer, g.load_range);
        }
        int budget = std::max(g.loads_per_frame, 1);
        const int max_in_flight = async ? maxInFlight(g) : std::max(g.max_loading, 1);
        syncGrid(world, store, e, state, sources, save_on_unload, budget, frame_budget, max_in_flight, async);
    }
    // Rejillas que ya no estan (borradas, otra escena): fuera sus teselas.
    for (auto it = grids_.begin(); it != grids_.end();) {
        if (it->second.alive) {
            ++it;
            continue;
        }
        for (auto& [key, tile] : it->second.tiles) {
            unload(world, store, it->second, key.first, key.second, tile, save_on_unload, async);
        }
        it = grids_.erase(it);
    }
    stats_.tiles_loaded = 0;
    stats_.tiles_loading = 0;
    stats_.bytes = 0;
    for (auto& [uuid, state] : grids_) {
        for (auto& [key, tile] : state.tiles) {
            if (tile.loading) ++stats_.tiles_loading;
            if (!tileAlive(world, tile)) continue;
            ++stats_.tiles_loaded;
            if (const auto data = store.findPath(pathOf(state, key.first, key.second, tile))) {
                stats_.bytes += dataBytes(*data);
            }
        }
    }
    stats_.saving = static_cast<int>(store.savingCount());
    stats_.main_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void TerrainStreamer::loadNow(ecs::World& world, TerrainStore& store, const Vec3& p, float range) {
    for (const entt::entity h : world.registry().view<TerrainGrid>()) {
        ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const TerrainGrid& g = e.get<TerrainGrid>();
        if (paused_folders_.count(g.folder) != 0) continue;
        GridState& state = grids_[e.uuid()];
        if (!sameLayout(state.settings, g)) {
            // Rutas cacheadas de otra carpeta: fuera (no deberia haber nada).
            for (auto& [key, tile] : state.tiles) cancel(tile);
            for (auto& [key, tile] : state.tiles) tile.path.clear();
        }
        state.settings = g;
        state.alive = true;
        const Vec3 corner = e.worldPosition();
        const float r = range > 0.0f ? range : std::min(g.load_range, g.tile_size);
        for (int z = 0; z < g.tiles_z; ++z) {
            for (int x = 0; x < g.tiles_x; ++x) {
                if (distanceToTile(p, corner, std::max(g.tile_size, 1.0f), x, z) > r) continue;
                Tile& tile = state.tiles[{x, z}];
                if (tileAlive(world, tile)) continue;
                const std::string& path = pathOf(state, x, z, tile);
                // Lo que estuviera leyendose en otro hilo se cancela: aqui se
                // espera al disco en este hilo (antes de empezar la partida).
                if (tile.loading) cancel(tile);
                if (!store.findPath(path)) {
                    if (std::shared_ptr<TerrainData> saved = store.takeSaving(path)) {
                        store.put(path, std::move(saved));
                    } else {
                        auto data = std::make_shared<TerrainData>();
                        std::error_code ec;
                        const std::filesystem::path file = store.absolute(path);
                        if (!std::filesystem::exists(file, ec) || !data->load(file)) {
                            data->create(g.tileResolution(),
                                         static_cast<std::uint32_t>(std::clamp(g.splat_resolution, 16, 4096)));
                            data->setSaved();
                        }
                        store.put(path, std::move(data));
                    }
                }
                spawn(world, e, state, x, z, tile);
                stitchFromNeighbours(store, g, x, z);
                if (const auto data = store.findPath(path)) tile.seen_version = data->version();
            }
        }
    }
}

void TerrainStreamer::clear(ecs::World& world, TerrainStore& store, bool save) {
    for (auto& [uuid, state] : grids_) {
        for (auto& [key, tile] : state.tiles) {
            // Cerrar la escena: lo que se guarde, ya (sincrono, como antes).
            unload(world, store, state, key.first, key.second, tile, save, false);
        }
    }
    grids_.clear();
    // Las escrituras de teselas soltadas antes, terminadas.
    if (save) store.flushSaves();
    stats_ = TerrainStreamStats{};
}

std::vector<std::pair<std::pair<int, int>, bool>> TerrainStreamer::tiles(const Uuid& grid) const {
    std::vector<std::pair<std::pair<int, int>, bool>> out;
    const auto it = grids_.find(grid);
    if (it == grids_.end()) return out;
    for (const auto& [key, tile] : it->second.tiles) out.emplace_back(key, !tile.loading && tile.entity != entt::null);
    return out;
}

void TerrainStreamer::pauseFolder(const std::string& folder, bool paused) {
    if (paused) {
        paused_folders_.insert(folder);
    } else {
        paused_folders_.erase(folder);
    }
}

}  // namespace cramion::terrain
