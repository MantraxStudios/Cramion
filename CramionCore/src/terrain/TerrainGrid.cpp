#include "CramionCore/terrain/TerrainGrid.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/foliage/Foliage.h"
#include "CramionCore/world/WorldPartition.h"

#include <algorithm>
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

int stitchTileBorders(TerrainStore& store, const TerrainGrid& grid, int x, int z) {
    const std::shared_ptr<TerrainData> src = store.findPath(tilePath(grid, x, z));
    if (!src) return 0;
    const int n = static_cast<int>(src->resolution());
    const int last = n - 1;
    int changed = 0;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const int nx = x + dx, nz = z + dz;
            if (nx < 0 || nz < 0 || nx >= grid.tiles_x || nz >= grid.tiles_z) continue;
            const std::shared_ptr<TerrainData> dst = store.findPath(tilePath(grid, nx, nz));
            if (!dst || static_cast<int>(dst->resolution()) != n) continue;
            // Vertices compartidos: la columna/fila del borde comun (o la esquina).
            const int sx0 = dx < 0 ? 0 : dx > 0 ? last : 0, sx1 = dx < 0 ? 0 : dx > 0 ? last : last;
            const int sz0 = dz < 0 ? 0 : dz > 0 ? last : 0, sz1 = dz < 0 ? 0 : dz > 0 ? last : last;
            int x0 = n, z0 = n, x1 = -1, z1 = -1;
            for (int sz = sz0; sz <= sz1; ++sz) {
                for (int sx = sx0; sx <= sx1; ++sx) {
                    const int tx = sx - dx * last, tz = sz - dz * last;
                    const float value = src->heights()[static_cast<std::size_t>(sz) * n + sx];
                    float& target = dst->heights()[static_cast<std::size_t>(tz) * n + tx];
                    if (target == value) continue;
                    target = value;
                    x0 = std::min(x0, tx);
                    z0 = std::min(z0, tz);
                    x1 = std::max(x1, tx);
                    z1 = std::max(z1, tz);
                }
            }
            if (x1 >= 0) {
                dst->markHeights(x0, z0, x1, z1);
                dst->commitCollision();
                ++changed;
            }
        }
    }
    return changed;
}

// -----------------------------------------------------------------------------
// Streaming
// -----------------------------------------------------------------------------

TerrainStreamer::~TerrainStreamer() {
    // Las lecturas en marcha terminan antes de destruir los futures.
    for (auto& [uuid, state] : grids_) {
        for (auto& [key, tile] : state.tiles) {
            if (tile.job.valid()) tile.job.wait();
        }
    }
}

bool TerrainStreamer::tileAlive(ecs::World& world, const Tile& tile) const {
    return tile.entity != entt::null && world.valid(tile.entity) && world.registry().all_of<TerrainTile>(tile.entity);
}

void TerrainStreamer::startLoad(TerrainStore& store, const GridState& state, int x, int z, Tile& tile) {
    const std::filesystem::path file = store.absolute(tilePath(state.settings, x, z));
    const std::uint32_t res = state.settings.tileResolution();
    const auto splat = static_cast<std::uint32_t>(std::clamp(state.settings.splat_resolution, 16, 4096));
    tile.loading = true;
    tile.job = std::async(std::launch::async, [file, res, splat]() {
        auto data = std::make_shared<TerrainData>();
        std::error_code ec;
        if (!std::filesystem::exists(file, ec) || !data->load(file)) {
            data->create(res, splat);  // tesela nueva: plana (se guarda al editarla)
            data->setSaved();
        }
        return data;
    });
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
    t.data = tilePath(state.settings, x, z);
    applyGridSettings(state.settings, t);
    if (const foliage::Grass* grass = grid_entity.tryGet<foliage::Grass>()) e.add<foliage::Grass>(*grass);
    e.add<TerrainTile>(TerrainTile{grid_entity.uuid(), x, z});
    tile.entity = e.handle();
    ++stats_.loads;
}

void TerrainStreamer::unload(ecs::World& world, TerrainStore& store, GridState& state, int x, int z, Tile& tile,
                             bool save) {
    if (tileAlive(world, tile)) world.destroy(world.wrap(tile.entity));
    tile.entity = entt::null;
    const std::string path = tilePath(state.settings, x, z);
    if (const std::shared_ptr<TerrainData> data = store.findPath(path)) {
        if (data->unsaved()) {
            // Con cambios: se guardan (editor) o se quedan en memoria hasta
            // que se guarde la escena (Play: no se pierde lo esculpido).
            if (!save) return;
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
                               const std::vector<std::pair<Vec3, float>>& sources, bool save_on_unload, int& budget) {
    const TerrainGrid& g = state.settings;
    const Vec3 corner = grid_entity.worldPosition();
    const float size = std::max(g.tile_size, 1.0f);
    const bool everything = sources.empty();  // sin streaming: todas
    // Lecturas terminadas: los datos al almacen (la entidad entra con el presupuesto).
    for (auto& [key, tile] : state.tiles) {
        if (!tile.loading || !tile.job.valid()) continue;
        if (tile.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        std::shared_ptr<TerrainData> data = tile.job.get();
        tile.loading = false;
        if (data) store.put(tilePath(g, key.first, key.second), std::move(data));
    }
    int reading = 0;
    for (const auto& [key, tile] : state.tiles) reading += tile.loading ? 1 : 0;

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
            } else if (present && best > g.unload_margin && !it->second.loading) {
                unload(world, store, state, x, z, it->second, save_on_unload);
                state.tiles.erase(it);
            }
        }
    }
    std::sort(wanted.begin(), wanted.end(), [](const Want& a, const Want& b) { return a.distance < b.distance; });
    for (const Want& w : wanted) {
        Tile& tile = state.tiles[{w.x, w.z}];
        if (tileAlive(world, tile) || tile.loading) continue;
        const std::string path = tilePath(g, w.x, w.z);
        if (store.findPath(path)) {
            if (budget <= 0) continue;
            spawn(world, grid_entity, state, w.x, w.z, tile);
            if (const auto data = store.findPath(path)) tile.seen_version = data->version();
            --budget;
        } else if (reading < std::max(g.max_loading, 1)) {
            startLoad(store, state, w.x, w.z, tile);
            ++reading;
        }
    }
    // Tras esculpir una tesela (el trazo ya termino: colision al dia), sus
    // bordes pasan a las vecinas.
    for (auto& [key, tile] : state.tiles) {
        if (!tileAlive(world, tile)) continue;
        const std::shared_ptr<TerrainData> data = store.findPath(tilePath(g, key.first, key.second));
        if (!data || data->version() == tile.seen_version || data->collisionVersion() != data->version()) continue;
        stitchTileBorders(store, g, key.first, key.second);
        tile.seen_version = data->version();
        // Las vecinas cambiadas no tienen que coser de vuelta.
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                auto n = state.tiles.find({key.first + dx, key.second + dz});
                if (n == state.tiles.end()) continue;
                if (const auto nd = store.findPath(tilePath(g, n->first.first, n->first.second))) {
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
    for (auto& [uuid, state] : grids_) state.alive = false;
    stats_.grids = 0;
    stats_.tiles_total = 0;
    for (const entt::entity h : world.registry().view<TerrainGrid>()) {
        ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const TerrainGrid& g = e.get<TerrainGrid>();
        GridState& state = grids_[e.uuid()];
        if (!sameLayout(state.settings, g) && !state.tiles.empty()) {
            // Otra carpeta o tamano: lo cargado ya no vale.
            for (auto& [key, tile] : state.tiles) {
                if (tile.job.valid()) tile.job.wait();
                unload(world, store, state, key.first, key.second, tile, save_on_unload);
            }
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
        syncGrid(world, store, e, state, sources, save_on_unload, budget);
    }
    // Rejillas que ya no estan (borradas, otra escena): fuera sus teselas.
    for (auto it = grids_.begin(); it != grids_.end();) {
        if (it->second.alive) {
            ++it;
            continue;
        }
        for (auto& [key, tile] : it->second.tiles) {
            if (tile.job.valid()) tile.job.wait();
            unload(world, store, it->second, key.first, key.second, tile, save_on_unload);
        }
        it = grids_.erase(it);
    }
    stats_.tiles_loaded = 0;
    stats_.tiles_loading = 0;
    stats_.bytes = 0;
    for (const auto& [uuid, state] : grids_) {
        for (const auto& [key, tile] : state.tiles) {
            if (tile.loading) ++stats_.tiles_loading;
            if (!tileAlive(world, tile)) continue;
            ++stats_.tiles_loaded;
            if (const auto data = store.findPath(tilePath(state.settings, key.first, key.second))) {
                stats_.bytes += dataBytes(*data);
            }
        }
    }
}

void TerrainStreamer::loadNow(ecs::World& world, TerrainStore& store, const Vec3& p, float range) {
    for (const entt::entity h : world.registry().view<TerrainGrid>()) {
        ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const TerrainGrid& g = e.get<TerrainGrid>();
        GridState& state = grids_[e.uuid()];
        state.settings = g;
        state.alive = true;
        const Vec3 corner = e.worldPosition();
        const float r = range > 0.0f ? range : std::min(g.load_range, g.tile_size);
        for (int z = 0; z < g.tiles_z; ++z) {
            for (int x = 0; x < g.tiles_x; ++x) {
                if (distanceToTile(p, corner, std::max(g.tile_size, 1.0f), x, z) > r) continue;
                Tile& tile = state.tiles[{x, z}];
                if (tileAlive(world, tile)) continue;
                const std::string path = tilePath(g, x, z);
                if (!store.findPath(path)) {
                    if (!tile.loading) startLoad(store, state, x, z, tile);
                    std::shared_ptr<TerrainData> data = tile.job.get();
                    tile.loading = false;
                    if (data) store.put(path, std::move(data));
                }
                spawn(world, e, state, x, z, tile);
                if (const auto data = store.findPath(path)) tile.seen_version = data->version();
            }
        }
    }
}

void TerrainStreamer::clear(ecs::World& world, TerrainStore& store, bool save) {
    for (auto& [uuid, state] : grids_) {
        for (auto& [key, tile] : state.tiles) {
            if (tile.job.valid()) tile.job.wait();
            unload(world, store, state, key.first, key.second, tile, save);
        }
    }
    grids_.clear();
    stats_ = TerrainStreamStats{};
}

std::vector<std::pair<std::pair<int, int>, bool>> TerrainStreamer::tiles(const Uuid& grid) const {
    std::vector<std::pair<std::pair<int, int>, bool>> out;
    const auto it = grids_.find(grid);
    if (it == grids_.end()) return out;
    for (const auto& [key, tile] : it->second.tiles) out.emplace_back(key, !tile.loading && tile.entity != entt::null);
    return out;
}

}  // namespace cramion::terrain
