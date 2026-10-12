#include "CramionCore/world/WorldPartition.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/environment/Environment.h"
#include "CramionCore/foliage/Foliage.h"
#include "CramionCore/net/NetworkObject.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/terrain/TerrainGrid.h"
#include "CramionCore/ui/UI.h"
#include "CramionCore/water/Water.h"
#include "CramionCore/xr/XrRig.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

namespace cramion::worldpart {

using core::Vec3;

namespace {
WorldPartitionSystem* g_active = nullptr;
}

WorldPartitionSystem* activePartition() { return g_active; }
void setActivePartition(WorldPartitionSystem* system) { g_active = system; }

void WorldPartition::reflect(ecs::PropertyVisitor& v) {
    v.field({"enabled", "Activo", "En Play y en el juego solo se cargan las celdas cerca del jugador"}, enabled);
    v.field({"cell_size", "Tamano de celda"}, cell_size, ecs::FloatRange{16.0f, 4096.0f, 1.0f, "%.0f m"});
    v.field({"load_range", "Distancia de carga"}, load_range, ecs::FloatRange{16.0f, 20000.0f, 1.0f, "%.0f m"});
    v.field({"unload_margin", "Margen de descarga", "Se descarga a la distancia de carga mas esto (sin parpadeos)"},
            unload_margin, ecs::FloatRange{0.0f, 2000.0f, 1.0f, "%.0f m"});
    v.field({"loads_per_frame", "Celdas por frame", "Celdas que se recrean cada frame (menos = sin tirones)"},
            loads_per_frame, 1, 64);
    v.field({"unloads_per_frame", "Descargas por frame", "Celdas que se guardan y quitan cada frame (menos = sin tirones)"},
            unloads_per_frame, 1, 64);
    v.field({"restores_per_frame", "Objetos por frame", "Objetos que se recrean cada frame al cargar una celda"},
            restores_per_frame, 8, 100000);
    v.field({"external_cells", "Celdas en disco",
             "Al guardar, los objetos que se descargan van a <escena>.cells/ (un archivo por celda) y no a la escena: "
             "el juego y el servidor solo leen las celdas cercanas (mundos grandes, MMO)"},
            external_cells);
    v.field({"show_grid", "Mostrar la rejilla"}, show_grid);
}

void StreamingSource::reflect(ecs::PropertyVisitor& v) {
    v.field({"enabled", "Activa"}, enabled);
    v.field({"range_scale", "Escala de la distancia"}, range_scale, ecs::FloatRange{0.1f, 10.0f, 0.01f, "%.2f"});
}

void AlwaysLoaded::reflect(ecs::PropertyVisitor& v) { v.field({"enabled", "Siempre cargado"}, enabled); }

void HlodProxy::reflect(ecs::PropertyVisitor& v) {
    v.field({"cell_x", "Celda X"}, cell_x, -100000, 100000);
    v.field({"cell_z", "Celda Z"}, cell_z, -100000, 100000);
}

void registerWorldPartitionComponents() {
    auto& r = ecs::ComponentRegistry::instance();
    if (r.find("WorldPartition") != nullptr) return;
    r.registerComponent<WorldPartition>("WorldPartition", "World Partition", "Mundo");
    r.registerComponent<StreamingSource>("StreamingSource", "Fuente de carga (World Partition)", "Mundo");
    r.registerComponent<AlwaysLoaded>("AlwaysLoaded", "Siempre cargado (World Partition)", "Mundo");
    r.registerComponent<HlodProxy>("HlodProxy", "HLOD de una celda", "Mundo");
}

namespace {

// Algo de la entidad o de sus hijos que no se puede descargar.
bool pinned(const ecs::Entity& e, bool& has_mesh, int depth = 0) {
    if (depth > 64) return true;
    if (const AlwaysLoaded* a = e.tryGet<AlwaysLoaded>(); a != nullptr && a->enabled) return true;
    if (e.has<ecs::Camera>() || e.has<terrain::Terrain>() || e.has<water::WaterBody>() ||
        e.has<environment::Environment>() || e.has<ui::Canvas>() || e.has<physics::CharacterController>() ||
        e.has<xr::XrOrigin>() || e.has<xr::XrPlayer>() || e.has<net::NetworkObject>() || e.has<StreamingSource>() ||
        e.has<WorldPartition>() || e.has<terrain::TerrainGrid>() || e.has<foliage::Foliage>() || e.has<foliage::Grass>() || e.has<HlodProxy>()) {
        return true;
    }
    if (const ecs::Light* l = e.tryGet<ecs::Light>(); l != nullptr && l->type == ecs::LightType::Directional) return true;
    if (e.has<ecs::MeshRenderer>()) has_mesh = true;
    for (const entt::entity c : e.children()) {
        if (pinned(e.world()->wrap(c), has_mesh, depth + 1)) return true;
    }
    return false;
}

}  // namespace

bool WorldPartitionSystem::streamable(const ecs::Entity& root) {
    if (!root.valid() || root.parent().valid()) return false;
    if (root.compareTag("Player")) return false;
    bool has_mesh = false;
    if (pinned(root, has_mesh)) return false;
    return has_mesh;
}

CellCoord WorldPartitionSystem::cellOf(const Vec3& p) const {
    const float s = std::max(settings_.cell_size, 1.0f);
    return CellCoord{static_cast<int>(std::floor(p.x / s)), static_cast<int>(std::floor(p.z / s))};
}

float WorldPartitionSystem::distanceToCell(const Vec3& p, const CellCoord& c) const {
    const float s = std::max(settings_.cell_size, 1.0f);
    const float x0 = static_cast<float>(c.x) * s;
    const float z0 = static_cast<float>(c.z) * s;
    const float dx = std::max({x0 - p.x, 0.0f, p.x - (x0 + s)});
    const float dz = std::max({z0 - p.z, 0.0f, p.z - (z0 + s)});
    return std::sqrt(dx * dx + dz * dz);
}

std::vector<std::pair<Vec3, float>> WorldPartitionSystem::sources(ecs::World& world, const Vec3& viewer) const {
    std::vector<std::pair<Vec3, float>> out;
    for (const entt::entity h : world.registry().view<StreamingSource>()) {
        const ecs::Entity e = world.wrap(h);
        const StreamingSource& s = e.get<StreamingSource>();
        if (!s.enabled || !e.activeInHierarchy()) continue;
        out.emplace_back(e.worldPosition(), settings_.load_range * std::max(s.range_scale, 0.01f));
    }
    if (out.empty()) out.emplace_back(viewer, settings_.load_range);
    return out;
}

void WorldPartitionSystem::track(entt::entity root, const CellCoord& c) {
    tracked_[root] = c;
    cells_[c].roots.push_back(root);
}

void WorldPartitionSystem::untrack(entt::entity root) { tracked_.erase(root); }

void WorldPartitionSystem::rescan(ecs::World& world) {
    // De vez en cuando se vuelven a mirar las rechazadas (les pusieron malla).
    if (++rescans_ % 64 == 0) rejected_.clear();
    std::unordered_map<entt::entity, CellCoord> next;
    next.reserve(tracked_.size() + 16);
    for (const entt::entity h : world.roots()) {
        if (tracked_.find(h) == tracked_.end()) {
            if (rejected_.count(h) != 0) continue;
            if (!streamable(world.wrap(h))) {
                rejected_.insert(h);
                continue;
            }
        }
        next.emplace(h, cellOf(world.wrap(h).worldPosition()));
    }
    tracked_ = std::move(next);
    for (auto& [coord, cell] : cells_) cell.roots.clear();
    for (const auto& [h, c] : tracked_) cells_[c].roots.push_back(h);
    for (auto& [coord, cell] : cells_) {
        if (cell.loaded) cell.entities = static_cast<int>(cell.roots.size());
    }
}

// -----------------------------------------------------------------------------
// Archivos de celda: "#CramionCell 1 <raices>" y una raiz por linea (el JSON de
// serializeEntity va en una sola linea).
// -----------------------------------------------------------------------------

namespace {

constexpr const char* kCellHeader = "#CramionCell 1 ";

bool writeCellFile(const std::filesystem::path& file, const std::vector<std::string>& roots, std::size_t* bytes) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << kCellHeader << roots.size() << '\n';
        for (const std::string& text : roots) out << text << '\n';
        if (!out) return false;
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(file, ec);
        std::filesystem::rename(temp, file, ec);
    }
    if (ec) return false;
    if (bytes != nullptr) *bytes = static_cast<std::size_t>(std::filesystem::file_size(file, ec));
    return true;
}

std::vector<std::string> readCellFile(const std::filesystem::path& file) {
    std::vector<std::string> roots;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        roots.push_back(std::move(line));
    }
    return roots;
}

// Raices que dice la cabecera (sin leer el archivo entero).
int cellFileCount(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!std::getline(in, line) || line.rfind(kCellHeader, 0) != 0) return 0;
    return std::atoi(line.c_str() + std::strlen(kCellHeader));
}

// "cell_X_Z.crcell" -> X, Z.
bool parseCellName(const std::filesystem::path& file, CellCoord& c) {
    const std::string name = file.filename().string();
    int x = 0, z = 0;
    char tail[16] = {};
    if (std::sscanf(name.c_str(), "cell_%d_%d.%15s", &x, &z, tail) != 3 || std::strcmp(tail, "crcell") != 0) return false;
    c = CellCoord{x, z};
    return true;
}

std::string cellName(const CellCoord& c) { return "cell_" + std::to_string(c.x) + "_" + std::to_string(c.z) + ".crcell"; }

}  // namespace

std::filesystem::path WorldPartitionSystem::cellsFolder(const std::filesystem::path& scene) {
    std::filesystem::path folder = scene.parent_path();
    folder /= scene.stem();
    folder += ".cells";
    return folder;
}

bool WorldPartitionSystem::hasExternalCells(const std::filesystem::path& scene) {
    if (scene.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_directory(cellsFolder(scene), ec);
}

int WorldPartitionSystem::writeExternalCells(ecs::World& world, float cell_size, const std::filesystem::path& scene,
                                             std::string* error) {
    const float s = std::max(cell_size, 1.0f);
    std::map<CellCoord, std::vector<std::string>> cells;
    for (const entt::entity h : world.roots()) {
        const ecs::Entity e = world.wrap(h);
        if (e.get<ecs::EntityInfo>().transient || !streamable(e)) continue;
        const Vec3 p = e.worldPosition();
        const CellCoord c{static_cast<int>(std::floor(p.x / s)), static_cast<int>(std::floor(p.z / s))};
        cells[c].push_back(ecs::serializeEntity(world, e));
    }
    const std::filesystem::path folder = cellsFolder(scene);
    std::error_code ec;
    for (const auto& [c, roots] : cells) {
        if (!writeCellFile(folder / cellName(c), roots, nullptr)) {
            if (error != nullptr) *error = "no se pudo escribir " + (folder / cellName(c)).string();
            return -1;
        }
    }
    // Las celdas que se quedaron vacias.
    if (std::filesystem::is_directory(folder, ec)) {
        std::vector<std::filesystem::path> stale;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            CellCoord c;
            if (parseCellName(entry.path(), c) && cells.count(c) == 0) stale.push_back(entry.path());
        }
        for (const std::filesystem::path& file : stale) std::filesystem::remove(file, ec);
    }
    return static_cast<int>(cells.size());
}

void WorldPartitionSystem::removeExternalCells(const std::filesystem::path& scene) {
    if (scene.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(cellsFolder(scene), ec);
}

int WorldPartitionSystem::loadExternalCells(ecs::World& world, const std::filesystem::path& scene) {
    if (!hasExternalCells(scene)) return 0;
    int count = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(cellsFolder(scene), ec)) {
        CellCoord c;
        if (!parseCellName(entry.path(), c)) continue;
        for (const std::string& text : readCellFile(entry.path())) {
            if (ecs::restoreEntities(world, text).valid()) ++count;
        }
    }
    return count;
}

// -----------------------------------------------------------------------------
// Sistema
// -----------------------------------------------------------------------------

WorldPartitionSystem::~WorldPartitionSystem() {
    for (auto& [coord, cell] : cells_) {
        if (cell.job.valid()) cell.job.wait();
    }
    removeCache();
}

void WorldPartitionSystem::setSceneFile(const std::filesystem::path& scene, bool cells_in_world) {
    scene_file_ = scene;
    cells_in_world_ = cells_in_world;
}

std::filesystem::path WorldPartitionSystem::cacheFile(const CellCoord& c) {
    if (cache_.empty()) {
        const std::filesystem::path root =
            cache_root_.empty() ? std::filesystem::temp_directory_path() / "Cramion" / "WorldPartition" : cache_root_;
        cache_ = root / Uuid::generate().toString();
    }
    return cache_ / cellName(c);
}

void WorldPartitionSystem::removeCache() {
    if (cache_.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(cache_, ec);
    cache_.clear();
}

void WorldPartitionSystem::registerExternalCells(ecs::World& world) {
    if (cells_in_world_ || !hasExternalCells(scene_file_)) return;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(cellsFolder(scene_file_), ec)) {
        CellCoord c;
        if (!parseCellName(entry.path(), c)) continue;
        Cell& cell = cells_[c];
        if (!cell.roots.empty()) {
            // Ya hay objetos de esta celda en la escena: la celda entera ya.
            for (const std::string& text : readCellFile(entry.path())) {
                const ecs::Entity r = ecs::restoreEntities(world, text);
                if (r.valid() && !r.parent().valid()) track(r.handle(), cellOf(r.worldPosition()));
            }
            continue;
        }
        cell.loaded = false;
        cell.file = entry.path();
        cell.disk_bytes = static_cast<std::size_t>(entry.file_size(ec));
        cell.entities = cellFileCount(entry.path());
        ++stats_.external_cells;
    }
}

void WorldPartitionSystem::begin(ecs::World& world, const Vec3& viewer) {
    registerWorldPartitionComponents();
    for (auto& [coord, cell] : cells_) {
        if (cell.job.valid()) cell.job.wait();
    }
    cells_.clear();
    tracked_.clear();
    rejected_.clear();
    removeCache();
    frame_ = 0;
    rescans_ = 0;
    stats_ = PartitionStats{};
    active_ = false;
    bool found = false;
    for (const entt::entity h : world.registry().view<WorldPartition>()) {
        const ecs::Entity e = world.wrap(h);
        const WorldPartition& wp = e.get<WorldPartition>();
        if (!wp.enabled || !e.activeInHierarchy()) continue;
        settings_ = wp;
        found = true;
        break;
    }
    if (!found) {
        // Sin World Partition activo, las celdas en disco entran todas.
        if (!cells_in_world_ && hasExternalCells(scene_file_)) {
            const int n = loadExternalCells(world, scene_file_);
            std::cout << "[WorldPartition] Sin World Partition activo: " << n << " objetos de las celdas en disco cargados\n";
        }
        return;
    }
    active_ = true;
    // Raices que se pueden descargar, por celda, y las celdas en disco.
    rescan(world);
    registerExternalCells(world);
    stats_.streamed_entities = static_cast<int>(tracked_.size());
    for (const auto& [coord, cell] : cells_) {
        if (!cell.loaded) stats_.streamed_entities += cell.entities;
    }
    // Lo cercano, cargado ya (el jugador empieza con su alrededor completo);
    // lo lejano, fuera (memoria libre desde el primer frame).
    const auto srcs = sources(world, viewer);
    for (auto& [coord, cell] : cells_) {
        bool in_range = false;
        for (const auto& [pos, range] : srcs) in_range = in_range || distanceToCell(pos, coord) <= range;
        if (in_range && !cell.loaded) loadCellNow(world, cell);
        if (!in_range && cell.loaded) unloadCell(world, coord, cell);
    }
    updateProxies(world);
    proxies_dirty_ = false;
    refreshStats();
    std::cout << "[WorldPartition] " << stats_.cells << " celdas de " << settings_.cell_size << " m, "
              << stats_.streamed_entities << " objetos; descargados " << stats_.unloaded_entities;
    if (stats_.external_cells > 0) std::cout << " (" << stats_.external_cells << " celdas en disco)";
    std::cout << "\n";
}

void WorldPartitionSystem::unloadCell(ecs::World& world, const CellCoord& c, Cell& cell) {
    if (!cell.loaded || cell.loading()) return;
    std::vector<std::string> stored;
    for (const entt::entity h : cell.roots) {
        if (!world.valid(h)) {
            untrack(h);
            continue;
        }
        ecs::Entity r = world.wrap(h);
        // Se movio a otra celda o ya es hijo de algo: el proximo repaso la pone
        // en su sitio.
        if (r.parent().valid() || !(cellOf(r.worldPosition()) == c)) continue;
        // Le pusieron algo que no se descarga (una camara, un objeto de red).
        if (!streamable(r)) {
            untrack(h);
            rejected_.insert(h);
            continue;
        }
        stored.push_back(ecs::serializeEntity(world, r));
        untrack(h);
        world.destroy(r);
    }
    cell.roots.clear();
    cell.entities = static_cast<int>(stored.size());
    cell.stored.clear();
    cell.bytes = 0;
    cell.disk_bytes = 0;
    cell.file.clear();
    if (!stored.empty()) {
        // A disco (la cache de la sesion); si no se puede, en memoria.
        const std::filesystem::path file = cacheFile(c);
        std::size_t written = 0;
        if (writeCellFile(file, stored, &written)) {
            cell.file = file;
            cell.disk_bytes = written;
        } else {
            for (const std::string& text : stored) cell.bytes += text.size();
            cell.stored = std::move(stored);
        }
    }
    cell.loaded = false;
    proxies_dirty_ = true;
    ++stats_.unloads;
}

void WorldPartitionSystem::requestLoad(Cell& cell) {
    if (cell.loaded || cell.loading()) return;
    cell.pending.clear();
    cell.next = 0;
    if (!cell.stored.empty()) {
        cell.pending = std::move(cell.stored);
        cell.stored.clear();
        cell.bytes = 0;
    } else if (!cell.file.empty()) {
        cell.reading = true;
        cell.job = std::async(std::launch::async, [file = cell.file]() { return readCellFile(file); });
    }
    if (!cell.loading()) finishLoad(cell);  // vacia
}

void WorldPartitionSystem::finishLoad(Cell& cell) {
    cell.loaded = true;
    cell.pending.clear();
    cell.next = 0;
    cell.file.clear();
    cell.disk_bytes = 0;
    proxies_dirty_ = true;
    ++stats_.loads;
}

void WorldPartitionSystem::pumpLoads(ecs::World& world, int budget) {
    for (auto& [coord, cell] : cells_) {
        if (cell.reading && cell.job.valid() &&
            cell.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            cell.pending = cell.job.get();
            cell.next = 0;
            cell.reading = false;
            if (cell.pending.empty()) finishLoad(cell);
        }
    }
    // track() puede crear celdas: el map no mueve las que ya hay.
    for (auto& [coord, cell] : cells_) {
        if (cell.reading || cell.next >= cell.pending.size()) continue;
        while (budget > 0 && cell.next < cell.pending.size()) {
            const ecs::Entity r = ecs::restoreEntities(world, cell.pending[cell.next++]);
            if (r.valid() && !r.parent().valid()) track(r.handle(), cellOf(r.worldPosition()));
            --budget;
        }
        if (cell.next >= cell.pending.size()) finishLoad(cell);
        if (budget <= 0) break;
    }
}

void WorldPartitionSystem::loadCellNow(ecs::World& world, Cell& cell) {
    if (cell.loaded) return;
    if (!cell.loading()) requestLoad(cell);
    if (cell.loaded) return;
    if (cell.reading) {
        cell.pending = cell.job.get();
        cell.next = 0;
        cell.reading = false;
    }
    while (cell.next < cell.pending.size()) {
        const ecs::Entity r = ecs::restoreEntities(world, cell.pending[cell.next++]);
        if (r.valid() && !r.parent().valid()) track(r.handle(), cellOf(r.worldPosition()));
    }
    finishLoad(cell);
}

void WorldPartitionSystem::update(ecs::World& world, const Vec3& viewer) {
    if (!active_) return;
    ++frame_;
    // Lo creado, borrado o movido en Play: cada pocos frames y solo las raices
    // (antes, el arbol entero con cada hijo en cada frame).
    const bool rescanned = frame_ % 10 == 0;
    if (rescanned) rescan(world);
    const auto srcs = sources(world, viewer);
    // Lo que entra (las mas cercanas primero) y lo que sale (las mas lejanas
    // primero), cada cosa con su presupuesto por frame.
    std::vector<std::pair<float, CellCoord>> to_load;
    std::vector<std::pair<float, CellCoord>> to_unload;
    for (const auto& [coord, cell] : cells_) {
        float best = 1e30f;
        for (const auto& [pos, r] : srcs) best = std::min(best, distanceToCell(pos, coord) - r);
        if (!cell.loaded && !cell.loading() && best <= 0.0f) to_load.emplace_back(best, coord);
        if (cell.loaded && best > settings_.unload_margin) to_unload.emplace_back(best, coord);
    }
    if (!to_unload.empty()) {
        // Antes de guardar: que lo que se movio a estas celdas tambien salga.
        if (!rescanned) rescan(world);
        std::sort(to_unload.begin(), to_unload.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        const int budget = std::max(settings_.unloads_per_frame, 1);
        for (int i = 0; i < static_cast<int>(to_unload.size()) && i < budget; ++i) {
            const CellCoord c = to_unload[static_cast<std::size_t>(i)].second;
            unloadCell(world, c, cells_[c]);
        }
    }
    std::sort(to_load.begin(), to_load.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const int budget = std::max(settings_.loads_per_frame, 1);
    for (int i = 0; i < static_cast<int>(to_load.size()) && i < budget; ++i) {
        requestLoad(cells_[to_load[static_cast<std::size_t>(i)].second]);
    }
    pumpLoads(world, std::max(settings_.restores_per_frame, 1));
    if (proxies_dirty_) {
        updateProxies(world);
        proxies_dirty_ = false;
    }
    refreshStats();
}

void WorldPartitionSystem::updateProxies(ecs::World& world) const {
    for (const entt::entity h : world.registry().view<HlodProxy>()) {
        const HlodProxy& p = world.registry().get<HlodProxy>(h);
        const bool show = active_ && !cellLoaded(CellCoord{p.cell_x, p.cell_z});
        ecs::Entity e = world.wrap(h);
        std::vector<ecs::Entity> stack{e};
        while (!stack.empty()) {
            ecs::Entity x = stack.back();
            stack.pop_back();
            if (ecs::MeshRenderer* mr = x.tryGet<ecs::MeshRenderer>()) mr->visible = show;
            for (const entt::entity c : x.children()) stack.push_back(world.wrap(c));
        }
    }
}

void WorldPartitionSystem::loadAll(ecs::World& world) {
    for (auto& [coord, cell] : cells_) loadCellNow(world, cell);
    if (proxies_dirty_) {
        updateProxies(world);
        proxies_dirty_ = false;
    }
    refreshStats();
}

void WorldPartitionSystem::end(ecs::World& world, bool restore) {
    if (active_ && restore) loadAll(world);
    for (auto& [coord, cell] : cells_) {
        if (cell.job.valid()) cell.job.wait();
    }
    cells_.clear();
    tracked_.clear();
    rejected_.clear();
    removeCache();
    active_ = false;
    stats_ = PartitionStats{};
}

bool WorldPartitionSystem::cellLoaded(const CellCoord& c) const {
    const auto it = cells_.find(c);
    return it == cells_.end() || it->second.loaded;
}

std::vector<std::pair<CellCoord, bool>> WorldPartitionSystem::cells() const {
    std::vector<std::pair<CellCoord, bool>> out;
    out.reserve(cells_.size());
    for (const auto& [coord, cell] : cells_) out.emplace_back(coord, cell.loaded);
    return out;
}

void WorldPartitionSystem::refreshStats() {
    stats_.active = active_;
    stats_.cells = static_cast<int>(cells_.size());
    stats_.loaded_cells = 0;
    stats_.unloaded_entities = 0;
    stats_.stored_bytes = 0;
    stats_.disk_bytes = 0;
    stats_.loading_cells = 0;
    for (const auto& [coord, cell] : cells_) {
        if (cell.loaded) {
            ++stats_.loaded_cells;
        } else {
            stats_.unloaded_entities += cell.entities;
            stats_.stored_bytes += cell.bytes;
            stats_.disk_bytes += cell.disk_bytes;
            if (cell.loading()) ++stats_.loading_cells;
        }
    }
}

}  // namespace cramion::worldpart
