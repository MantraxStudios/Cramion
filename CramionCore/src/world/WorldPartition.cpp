#include "CramionCore/world/WorldPartition.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/environment/Environment.h"
#include "CramionCore/foliage/Foliage.h"
#include "CramionCore/jobs/JobSystem.h"
#include "CramionCore/net/NetworkObject.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/terrain/TerrainGrid.h"
#include "CramionCore/ui/UI.h"
#include "CramionCore/water/Water.h"
#include "CramionCore/xr/XrRig.h"

#include <algorithm>
#include <atomic>
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

cvar::CVar<bool> g_async_cells("wp.AsyncCells", true,
                               "World Partition: leer y parsear las celdas en otros hilos, recrear con presupuesto de "
                               "tiempo y escribir al descargar en el hilo de IO (apagado: como antes)",
                               cvar::Saved);
cvar::CVar<float> g_restore_budget_ms("wp.RestoreBudgetMs", 2.0f,
                                      "Milisegundos por frame para recrear objetos de las celdas que se cargan (siempre "
                                      "entra al menos uno; ademas del limite de objetos por frame)",
                                      cvar::Saved, 0.0f, 100.0f);
cvar::CVar<int> g_max_reads("wp.MaxIoInFlight", 8, "Celdas leyendose de disco a la vez en otros hilos", cvar::Saved, 1,
                            64);

std::string trim(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
}  // namespace

struct WorldPartitionSystem::PendingRead {
    jobs::Cancel cancel = std::make_shared<jobs::CancelToken>();
    std::atomic<bool> done{false};
    // Lo escribe el hilo antes de done = true; el principal lo lee despues.
    std::vector<PendingRoot> roots;
    bool ready() const { return done.load(std::memory_order_acquire); }
};

struct WorldPartitionSystem::PendingWrite {
    std::filesystem::path file;  // fijo antes de lanzar
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    std::size_t bytes = 0;  // escrito antes de done = true
    jobs::JobHandle job;
    bool ready() const { return done.load(std::memory_order_acquire); }
};

WorldPartitionSystem* activePartition() { return g_active; }
void setActivePartition(WorldPartitionSystem* system) { g_active = system; }

void PartitionGrid::reflect(ecs::PropertyVisitor& v) {
    v.field({"name", "Nombre", "El que pide el Objeto del World Partition en \"Rejilla\""}, name);
    v.field({"cell_size", "Tamano de celda"}, cell_size, ecs::FloatRange{16.0f, 16384.0f, 1.0f, "%.0f m"});
    v.field({"load_range", "Distancia de carga"}, load_range, ecs::FloatRange{16.0f, 50000.0f, 1.0f, "%.0f m"});
    v.field({"tag", "Etiqueta", "Los objetos con esta etiqueta van a esta rejilla (vacio = solo los que la piden)"}, tag);
}

void DataLayer::reflect(ecs::PropertyVisitor& v) {
    v.field({"name", "Nombre"}, name);
    v.field({"active", "Activa al empezar",
             "En Play y en el juego sus objetos se cargan (luego se cambia con WorldPartition.setDataLayer)"},
            active);
    v.field({"editor_visible", "Visible en el editor", "Sus objetos se ven en la Escena (no cambia la escena guardada)"},
            editor_visible);
}

void CellDependency::reflect(ecs::PropertyVisitor& v) {
    v.field({"grid", "Rejilla", "Rejilla de la celda (vacio = la principal)"}, grid);
    v.field({"x", "Celda X"}, x, -100000, 100000);
    v.field({"z", "Celda Z"}, z, -100000, 100000);
    v.field({"load_grid", "Carga en la rejilla", "Rejilla de la celda que se carga con ella (vacio = la principal)"},
            load_grid);
    v.field({"load_x", "Carga la celda X"}, load_x, -100000, 100000);
    v.field({"load_z", "Carga la celda Z"}, load_z, -100000, 100000);
}

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
    ecs::listField(v, {"grids", "Rejillas", "Rejillas ademas de la principal, cada una con su tamano y su distancia"},
                   grids, [](PartitionGrid& g, ecs::PropertyVisitor& item) { g.reflect(item); });
    ecs::listField(v, {"data_layers", "Data Layers", "Capas de objetos que se activan enteras (en el editor y en el juego)"},
                   data_layers, [](DataLayer& l, ecs::PropertyVisitor& item) { l.reflect(item); });
    ecs::listField(v, {"dependencies", "Dependencias entre celdas", "Cuando una celda se carga, tambien la otra"},
                   dependencies, [](CellDependency& d, ecs::PropertyVisitor& item) { d.reflect(item); });
}

void PartitionObject::reflect(ecs::PropertyVisitor& v) {
    v.field({"grid", "Rejilla", "Nombre de una rejilla del World Partition (vacio = la de su etiqueta o la principal)"},
            grid);
    v.field({"data_layers", "Data Layers",
             "Capas separadas por comas (\"Noche, Ruinas\"): solo se carga con todas activas. Vacio = ninguna"},
            data_layers);
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
    r.registerComponent<PartitionObject>("PartitionObject", "Objeto del World Partition (rejilla y capas)", "Mundo");
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
    if (has_mesh) return true;
    // Sin malla: solo si pide rejilla o capas (una luz, un sonido o un
    // disparador de una capa).
    const PartitionObject* po = root.tryGet<PartitionObject>();
    return po != nullptr && (!trim(po->data_layers).empty() || !trim(po->grid).empty());
}

// --- Rejillas y capas ----------------------------------------------------------

int WorldPartitionSystem::findGrid(const WorldPartition& settings, const std::string& name) {
    const std::string wanted = trim(name);
    if (wanted.empty()) return 0;
    for (std::size_t i = 0; i < settings.grids.size(); ++i) {
        if (trim(settings.grids[i].name) == wanted) return static_cast<int>(i) + 1;
    }
    return -1;
}

int WorldPartitionSystem::gridOf(const WorldPartition& settings, const ecs::Entity& root) {
    if (settings.grids.empty()) return 0;
    if (const PartitionObject* po = root.tryGet<PartitionObject>(); po != nullptr && !po->grid.empty()) {
        const int g = findGrid(settings, po->grid);
        if (g >= 0) return g;  // una que no existe: la de su etiqueta o la principal
    }
    for (std::size_t i = 0; i < settings.grids.size(); ++i) {
        const std::string& tag = settings.grids[i].tag;
        if (!tag.empty() && root.compareTag(tag)) return static_cast<int>(i) + 1;
    }
    return 0;
}

float WorldPartitionSystem::gridCellSize(const WorldPartition& settings, int grid) {
    if (grid <= 0 || grid > static_cast<int>(settings.grids.size())) return std::max(settings.cell_size, 1.0f);
    return std::max(settings.grids[static_cast<std::size_t>(grid - 1)].cell_size, 1.0f);
}

float WorldPartitionSystem::gridLoadRange(const WorldPartition& settings, int grid) {
    if (grid <= 0 || grid > static_cast<int>(settings.grids.size())) return settings.load_range;
    return settings.grids[static_cast<std::size_t>(grid - 1)].load_range;
}

std::string WorldPartitionSystem::gridName(const WorldPartition& settings, int grid) {
    if (grid <= 0 || grid > static_cast<int>(settings.grids.size())) return {};
    return trim(settings.grids[static_cast<std::size_t>(grid - 1)].name);
}

std::vector<std::string> WorldPartitionSystem::splitLayers(const std::string& canonical) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= canonical.size()) {
        const std::size_t comma = canonical.find(',', at);
        const std::string name = trim(canonical.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
        if (!name.empty()) out.push_back(name);
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return out;
}

std::string WorldPartitionSystem::canonicalLayers(const std::string& text) {
    std::vector<std::string> names = splitLayers(text);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::string out;
    for (const std::string& n : names) {
        if (!out.empty()) out += ',';
        out += n;
    }
    return out;
}

std::string WorldPartitionSystem::layersOf(const ecs::Entity& root) {
    const PartitionObject* po = root.tryGet<PartitionObject>();
    return po != nullptr ? canonicalLayers(po->data_layers) : std::string();
}

int WorldPartitionSystem::layerSetId(const std::string& canonical) {
    if (const auto it = layer_set_ids_.find(canonical); it != layer_set_ids_.end()) return it->second;
    const int id = static_cast<int>(layer_sets_.size());
    layer_sets_.push_back(canonical);
    layer_names_.push_back(splitLayers(canonical));
    layer_set_ids_.emplace(canonical, id);
    return id;
}

int WorldPartitionSystem::layerIdOf(const ecs::Entity& root) {
    const PartitionObject* po = root.tryGet<PartitionObject>();
    if (po == nullptr || po->data_layers.empty()) return 0;
    if (const auto it = raw_layer_ids_.find(po->data_layers); it != raw_layer_ids_.end()) return it->second;
    const int id = layerSetId(canonicalLayers(po->data_layers));
    raw_layer_ids_.emplace(po->data_layers, id);
    return id;
}

bool WorldPartitionSystem::layerSetActive(int id) const {
    if (id <= 0 || id >= static_cast<int>(layer_names_.size())) return true;
    // Los nombres ya vienen limpios: sin recortarlos otra vez (cada frame).
    for (const std::string& name : layer_names_[static_cast<std::size_t>(id)]) {
        const auto it = layer_state_.find(name);
        if (it != layer_state_.end() && !it->second) return false;
    }
    return true;
}

bool WorldPartitionSystem::dataLayerActive(const std::string& name) const {
    const auto it = layer_state_.find(trim(name));
    return it == layer_state_.end() || it->second;
}

void WorldPartitionSystem::setDataLayerActive(const std::string& name, bool active) {
    const std::string n = trim(name);
    if (n.empty()) return;
    layer_state_[n] = active;
    layer_overrides_[n] = active;
}

std::vector<std::pair<std::string, bool>> WorldPartitionSystem::dataLayers() const {
    std::map<std::string, bool> all;
    for (const DataLayer& l : settings_.data_layers) {
        const std::string n = trim(l.name);
        if (!n.empty()) all[n] = active_ ? dataLayerActive(n) : l.active;
    }
    for (const auto& [n, a] : layer_state_) all[n] = a;
    for (const auto& [n, a] : layer_overrides_) all[n] = a;
    return {all.begin(), all.end()};
}

// -----------------------------------------------------------------------------

CellCoord WorldPartitionSystem::cellAt(const ecs::DVec3& absolute, float cell_size) {
    const double s = std::max(static_cast<double>(cell_size), 1.0);
    return CellCoord{static_cast<int>(std::floor(absolute.x / s)), static_cast<int>(std::floor(absolute.z / s))};
}

// Las celdas son de la posicion absoluta: no cambian al moverse el origen.
CellCoord WorldPartitionSystem::cellOf(const Vec3& p) const { return cellOf(0, p); }

CellCoord WorldPartitionSystem::cellOf(int grid, const Vec3& p) const {
    return cellAt(ecs::DVec3{origin_.x + p.x, origin_.y + p.y, origin_.z + p.z}, gridCellSize(settings_, grid));
}

WorldPartitionSystem::CellKey WorldPartitionSystem::keyOf(const ecs::Entity& root) {
    const int grid = gridOf(settings_, root);
    const CellCoord c = cellOf(grid, root.worldPosition());
    return CellKey{grid, c.x, c.z, layerIdOf(root)};
}

float WorldPartitionSystem::distanceToCell(const Vec3& p, const CellKey& c) const {
    const double s = static_cast<double>(gridCellSize(settings_, c.grid));
    const double px = origin_.x + p.x;
    const double pz = origin_.z + p.z;
    const double x0 = static_cast<double>(c.x) * s;
    const double z0 = static_cast<double>(c.z) * s;
    const double dx = std::max({x0 - px, 0.0, px - (x0 + s)});
    const double dz = std::max({z0 - pz, 0.0, pz - (z0 + s)});
    return static_cast<float>(std::sqrt(dx * dx + dz * dz));
}

void WorldPartitionSystem::shiftOrigin(const Vec3& offset) {
    origin_.x += offset.x;
    origin_.y += offset.y;
    origin_.z += offset.z;
}

std::vector<std::pair<Vec3, float>> WorldPartitionSystem::sources(ecs::World& world, const Vec3& viewer) const {
    std::vector<std::pair<Vec3, float>> out;
    for (const entt::entity h : world.registry().view<StreamingSource>()) {
        const ecs::Entity e = world.wrap(h);
        const StreamingSource& s = e.get<StreamingSource>();
        if (!s.enabled || !e.activeInHierarchy()) continue;
        out.emplace_back(e.worldPosition(), std::max(s.range_scale, 0.01f));
    }
    if (out.empty()) out.emplace_back(viewer, 1.0f);
    return out;
}

std::vector<std::pair<WorldPartitionSystem::CellKey, WorldPartitionSystem::Desire>> WorldPartitionSystem::desires(
    const std::vector<std::pair<Vec3, float>>& srcs) const {
    std::vector<std::pair<CellKey, Desire>> out;
    out.reserve(cells_.size());
    // La distancia es la misma para todas las capas de un (rejilla, x, z):
    // van seguidas en cells_, se calcula una vez.
    float gap = 0.0f;
    bool have = false;
    CellKey last{};
    for (const auto& [key, cell] : cells_) {
        if (!have || key.grid != last.grid || key.x != last.x || key.z != last.z) {
            gap = 1e30f;
            const float range = gridLoadRange(settings_, key.grid);
            for (const auto& [pos, scale] : srcs) gap = std::min(gap, distanceToCell(pos, key) - range * scale);
            last = key;
            have = true;
        }
        Desire d;
        d.load = gap <= 0.0f;
        d.keep = gap <= settings_.unload_margin;
        d.priority = gap;
        out.emplace_back(key, d);
    }
    if (!deps_.empty()) {
        // Lo que pide una celda que se carga (o que se queda) tambien se
        // carga (o se queda), con su misma prioridad. En cadena.
        std::map<GridCoord, float> load;
        std::map<GridCoord, float> keep;
        const auto add = [](std::map<GridCoord, float>& set, const GridCoord& g, float p) {
            const auto [it, inserted] = set.emplace(g, p);
            if (!inserted) it->second = std::min(it->second, p);
        };
        for (const auto& [key, d] : out) {
            if (!layerSetActive(key.layers)) continue;  // una celda apagada no arrastra a nadie
            const GridCoord g{key.grid, key.x, key.z};
            if (d.load) add(load, g, d.priority);
            if (d.keep) add(keep, g, d.priority);
        }
        const auto spread = [this](std::map<GridCoord, float>& set) {
            std::vector<GridCoord> work;
            work.reserve(set.size());
            for (const auto& [g, p] : set) work.push_back(g);
            // Cada vuelta solo baja prioridades: acaba aunque haya bucles.
            while (!work.empty()) {
                const GridCoord g = work.back();
                work.pop_back();
                const auto deps = deps_.find(g);
                if (deps == deps_.end()) continue;
                const float p = set[g];
                for (const GridCoord& target : deps->second) {
                    const auto [it, inserted] = set.emplace(target, p);
                    if (inserted) {
                        work.push_back(target);
                    } else if (p < it->second) {
                        it->second = p;
                        work.push_back(target);
                    }
                }
            }
        };
        spread(load);
        spread(keep);
        for (auto& [key, d] : out) {
            const GridCoord g{key.grid, key.x, key.z};
            if (const auto it = load.find(g); it != load.end()) {
                d.load = true;
                d.priority = std::min(d.priority, it->second);
            }
            if (keep.count(g) != 0) d.keep = true;
        }
    }
    // Capas apagadas: ni se cargan ni se quedan (y salen las primeras).
    for (auto& [key, d] : out) {
        if (key.layers != 0 && !layerSetActive(key.layers)) {
            d.load = false;
            d.keep = false;
            d.priority = 1e30f;
        }
    }
    return out;
}

void WorldPartitionSystem::track(entt::entity root, const CellKey& c) {
    tracked_[root] = c;
    cells_[c].roots.push_back(root);
}

void WorldPartitionSystem::untrack(entt::entity root) { tracked_.erase(root); }

void WorldPartitionSystem::rescan(ecs::World& world) {
    // De vez en cuando se vuelven a mirar las rechazadas (les pusieron malla).
    ++rescans_;
    if (!g_async_cells.get()) {
        if (rescans_ % 64 == 0) rejected_.clear();
    } else if (!rejected_.empty()) {
        // Un trozo en cada repaso (1/64 de los cubos, en rueda) en vez de
        // todas de golpe cada 64: sin el pico de mirar todas las raices.
        const std::size_t buckets = rejected_.bucket_count();
        const std::size_t slice = buckets / 64 + 1;
        const std::size_t first = (static_cast<std::size_t>(rescans_) % 64) * slice;
        std::vector<entt::entity> forget;
        for (std::size_t b = first; b < std::min(buckets, first + slice); ++b) {
            for (auto it = rejected_.begin(b); it != rejected_.end(b); ++it) forget.push_back(*it);
        }
        for (const entt::entity h : forget) rejected_.erase(h);
    }
    std::unordered_map<entt::entity, CellKey> next;
    next.reserve(tracked_.size() + 16);
    for (const entt::entity h : world.roots()) {
        if (tracked_.find(h) == tracked_.end()) {
            if (rejected_.count(h) != 0) continue;
            if (!streamable(world.wrap(h))) {
                rejected_.insert(h);
                continue;
            }
        }
        next.emplace(h, keyOf(world.wrap(h)));
    }
    tracked_ = std::move(next);
    for (auto& [coord, cell] : cells_) cell.roots.clear();
    for (const auto& [h, c] : tracked_) cells_[c].roots.push_back(h);
    for (auto& [coord, cell] : cells_) {
        if (cell.loaded) cell.entities = static_cast<int>(cell.roots.size());
    }
    // Una raiz que entro en una celda descargada no se queda cargada para
    // siempre (al lado de su HLOD): esa celda se carga entera y, si esta
    // lejos, un update siguiente la descarga con ella dentro.
    for (auto& [coord, cell] : cells_) {
        if (!cell.loaded && !cell.loading() && !cell.roots.empty()) requestLoad(cell);
    }
}

// -----------------------------------------------------------------------------
// Archivos de celda: "#CramionCell 3 <raices> <origen x y z>", luego (si no
// es la rejilla principal o tiene capas) "#grid <nombre>" y "#layers <A,B>", y
// una raiz por linea (el JSON de serializeEntity va en una sola linea). El
// origen es el del mundo al guardar (las posiciones de las raices son
// relativas a el). Los de version 1 no lo llevan: se toma el origen del mundo
// al leerlos (el de la escena, con el que se guardaron). Los de version 2 no
// llevan rejilla ni capas: la principal, sin capas.
// -----------------------------------------------------------------------------

namespace {

constexpr const char* kCellMagic = "#CramionCell ";
constexpr const char* kCellExtension = ".crcell";

bool writeCellFile(const std::filesystem::path& file, const std::vector<std::string>& roots, const ecs::DVec3& origin,
                   std::size_t* bytes, const std::string& grid = {}, const std::string& layers = {}) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        char header[160];
        std::snprintf(header, sizeof(header), "%s3 %zu %.17g %.17g %.17g\n", kCellMagic, roots.size(), origin.x, origin.y,
                      origin.z);
        out << header;
        if (!grid.empty()) out << "#grid " << grid << '\n';
        if (!layers.empty()) out << "#layers " << layers << '\n';
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

// La cabecera (sin leer el archivo entero): cuantas raices, el origen con el
// que se guardaron (version 2+) y la rejilla y las capas (version 3).
struct CellMeta {
    int count = 0;
    bool has_origin = false;
    ecs::DVec3 origin{};
    std::string grid;
    std::string layers;
};

CellMeta readCellMeta(const std::filesystem::path& file) {
    CellMeta meta;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!std::getline(in, line) || line.rfind(kCellMagic, 0) != 0) return meta;
    int version = 0;
    int count = 0;
    double x = 0.0, y = 0.0, z = 0.0;
    const int n = std::sscanf(line.c_str() + std::strlen(kCellMagic), "%d %d %lf %lf %lf", &version, &count, &x, &y, &z);
    if (n >= 2) meta.count = count;
    if (version >= 2 && n >= 5) {
        meta.has_origin = true;
        meta.origin = ecs::DVec3{x, y, z};
    }
    // Las lineas '#' que siguen: rejilla y capas.
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] != '#') break;
        if (line.rfind("#grid ", 0) == 0) meta.grid = trim(line.substr(6));
        else if (line.rfind("#layers ", 0) == 0) meta.layers = WorldPartitionSystem::canonicalLayers(line.substr(8));
    }
    return meta;
}

// Deja una raiz recreada (guardada con el origen `saved`) en su sitio real
// con el origen de ahora.
ecs::Entity placeRestored(ecs::World& world, ecs::Entity r, const ecs::DVec3& saved) {
    if (!r.valid() || r.parent().valid() || saved == world.origin()) return r;
    const ecs::DVec3& now = world.origin();
    const Vec3 delta{static_cast<float>(saved.x - now.x), static_cast<float>(saved.y - now.y),
                     static_cast<float>(saved.z - now.z)};
    r.setLocalPosition(r.localPosition() + delta);
    return r;
}

// Recrea una raiz guardada con el origen `saved` y la deja en su sitio real
// con el origen de ahora. Un UUID que ya esta en el mundo no se recrea
// (restoreEntities avisa).
ecs::Entity restoreRoot(ecs::World& world, const std::string& text, const ecs::DVec3& saved) {
    return placeRestored(world, ecs::restoreEntities(world, text), saved);
}

// Una raiz pendiente (ya parseada en otro hilo, o el texto).
template <typename Pending>
ecs::Entity restorePending(ecs::World& world, const Pending& p, const ecs::DVec3& saved) {
    if (p.parsed) return placeRestored(world, ecs::restoreParsedEntities(world, *p.parsed), saved);
    return restoreRoot(world, p.text, saved);
}

// "cell_X_Z.crcell" o "cell_X_Z_<hash>.crcell" -> X, Z.
bool parseCellName(const std::filesystem::path& file, CellCoord& c) {
    const std::string name = file.filename().string();
    const std::size_t ext = std::strlen(kCellExtension);
    if (name.size() <= 5 + ext || name.compare(0, 5, "cell_") != 0 ||
        name.compare(name.size() - ext, ext, kCellExtension) != 0) {
        return false;
    }
    const std::string core = name.substr(5, name.size() - 5 - ext);
    int x = 0, z = 0, used = 0;
    if (std::sscanf(core.c_str(), "%d_%d%n", &x, &z, &used) != 2 || used <= 0) return false;
    const std::string rest = core.substr(static_cast<std::size_t>(used));
    if (!rest.empty()) {
        if (rest.size() != 9 || rest[0] != '_') return false;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            if (std::strchr("0123456789abcdef", rest[i]) == nullptr) return false;
        }
    }
    c = CellCoord{x, z};
    return true;
}

// La de la rejilla principal sin capas, como siempre; las demas llevan un
// hash de "rejilla|capas" (solo para no chocar: lo de verdad va en la cabecera).
std::string cellName(const CellCoord& c, const std::string& grid = {}, const std::string& layers = {}) {
    std::string name = "cell_" + std::to_string(c.x) + "_" + std::to_string(c.z);
    if (!grid.empty() || !layers.empty()) {
        std::uint32_t h = 2166136261u;  // FNV-1a
        for (const char ch : grid + "|" + layers) {
            h ^= static_cast<unsigned char>(ch);
            h *= 16777619u;
        }
        char hex[16];
        std::snprintf(hex, sizeof(hex), "_%08x", static_cast<unsigned>(h));
        name += hex;
    }
    return name + kCellExtension;
}

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
    WorldPartition settings;
    settings.cell_size = cell_size;
    return writeExternalCells(world, settings, scene, error);
}

int WorldPartitionSystem::writeExternalCells(ecs::World& world, const WorldPartition& settings,
                                             const std::filesystem::path& scene, std::string* error) {
    struct Out {
        std::string grid;
        std::string layers;
        std::vector<std::string> roots;
    };
    std::map<std::string, Out> cells;  // por nombre de archivo
    for (const entt::entity h : world.roots()) {
        const ecs::Entity e = world.wrap(h);
        if (e.get<ecs::EntityInfo>().transient || !streamable(e)) continue;
        const int grid = gridOf(settings, e);
        const CellCoord c = cellAt(world.absolute(e.worldPosition()), gridCellSize(settings, grid));
        const std::string grid_name = gridName(settings, grid);
        const std::string layers = layersOf(e);
        Out& out = cells[cellName(c, grid_name, layers)];
        out.grid = grid_name;
        out.layers = layers;
        out.roots.push_back(ecs::serializeEntity(world, e));
    }
    const std::filesystem::path folder = cellsFolder(scene);
    std::error_code ec;
    for (const auto& [name, out] : cells) {
        if (!writeCellFile(folder / name, out.roots, world.origin(), nullptr, out.grid, out.layers)) {
            if (error != nullptr) *error = "no se pudo escribir " + (folder / name).string();
            return -1;
        }
    }
    // Las celdas que se quedaron vacias.
    if (std::filesystem::is_directory(folder, ec)) {
        std::vector<std::filesystem::path> stale;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            CellCoord c;
            if (parseCellName(entry.path(), c) && cells.count(entry.path().filename().string()) == 0) {
                stale.push_back(entry.path());
            }
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
        const CellMeta meta = readCellMeta(entry.path());
        const ecs::DVec3 saved = meta.has_origin ? meta.origin : world.origin();  // version 1: el de la escena
        for (const std::string& text : readCellFile(entry.path())) {
            if (restoreRoot(world, text, saved).valid()) ++count;
        }
    }
    return count;
}

// -----------------------------------------------------------------------------
// Sistema
// -----------------------------------------------------------------------------

WorldPartitionSystem::~WorldPartitionSystem() {
    drainJobs();
    removeCache();
}

void WorldPartitionSystem::drainJobs() {
    // Las lecturas se cancelan (descartan lo leido); las escrituras terminan.
    // Ningun trabajo tiene `this`: solo su estado compartido.
    for (auto& [coord, cell] : cells_) {
        if (cell.read) cell.read->cancel->cancelled.store(true, std::memory_order_release);
    }
    for (const jobs::JobHandle& h : io_jobs_) jobs::wait(h);
    io_jobs_.clear();
}

int WorldPartitionSystem::readsInFlight() const {
    int n = 0;
    for (const auto& [coord, cell] : cells_) n += cell.reading ? 1 : 0;
    return n;
}

void WorldPartitionSystem::pollWrites() {
    io_jobs_.erase(std::remove_if(io_jobs_.begin(), io_jobs_.end(),
                                  [](const jobs::JobHandle& h) { return !h || h->done(); }),
                   io_jobs_.end());
    for (auto& [coord, cell] : cells_) {
        if (!cell.write || !cell.write->ready()) continue;
        const std::shared_ptr<PendingWrite> w = std::move(cell.write);
        cell.write.reset();
        // Sigue descargada y nadie la esta cargando: ya esta en disco, fuera
        // de la memoria. Si fallo, se queda en memoria (como antes).
        if (w->ok.load(std::memory_order_acquire) && !cell.loaded && !cell.loading() && cell.stored) {
            cell.file = w->file;
            cell.disk_bytes = w->bytes;
            cell.stored.reset();
            cell.bytes = 0;
        }
    }
}

void WorldPartitionSystem::setSceneFile(const std::filesystem::path& scene, bool cells_in_world) {
    scene_file_ = scene;
    cells_in_world_ = cells_in_world;
}

std::filesystem::path WorldPartitionSystem::cacheFile(const CellKey& c) {
    if (cache_.empty()) {
        const std::filesystem::path root =
            cache_root_.empty() ? std::filesystem::temp_directory_path() / "Cramion" / "WorldPartition" : cache_root_;
        cache_ = root / Uuid::generate().toString();
    }
    // Rejilla y capas en el nombre: dos celdas con el mismo x, z no chocan.
    return cache_ / ("cell_" + std::to_string(c.grid) + "_" + std::to_string(c.layers) + "_" + std::to_string(c.x) + "_" +
                     std::to_string(c.z) + kCellExtension);
}

void WorldPartitionSystem::removeCache() {
    if (cache_.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(cache_, ec);
    cache_.clear();
}

void WorldPartitionSystem::restoreTracked(ecs::World& world, const std::string& text, const ecs::DVec3& saved) {
    const ecs::Entity r = restoreRoot(world, text, saved);
    if (r.valid() && !r.parent().valid()) track(r.handle(), keyOf(r));
}

void WorldPartitionSystem::registerExternalCells(ecs::World& world) {
    if (cells_in_world_ || !hasExternalCells(scene_file_)) return;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(cellsFolder(scene_file_), ec)) {
        CellCoord c;
        if (!parseCellName(entry.path(), c)) continue;
        const CellMeta meta = readCellMeta(entry.path());
        // Version 1: el origen de la escena (con el que se guardo).
        const ecs::DVec3 saved = meta.has_origin ? meta.origin : world.origin();
        const int grid = findGrid(settings_, meta.grid);
        if (grid < 0) {
            // Su rejilla ya no existe: sus objetos entran ya y se reparten en
            // las rejillas de ahora.
            std::cerr << "[WorldPartition] " << entry.path().filename().string() << ": no hay rejilla \"" << meta.grid
                      << "\"; sus objetos se cargan ya\n";
            for (const std::string& text : readCellFile(entry.path())) restoreTracked(world, text, saved);
            continue;
        }
        const CellKey key{grid, c.x, c.z, layerSetId(meta.layers)};
        Cell& cell = cells_[key];
        if (!cell.roots.empty()) {
            // Ya hay objetos de esta celda en la escena: la celda entera ya.
            for (const std::string& text : readCellFile(entry.path())) restoreTracked(world, text, saved);
            continue;
        }
        cell.loaded = false;
        cell.file = entry.path();
        cell.origin = saved;
        cell.disk_bytes = static_cast<std::size_t>(entry.file_size(ec));
        cell.entities = meta.count;
        ++stats_.external_cells;
    }
}

void WorldPartitionSystem::begin(ecs::World& world, const Vec3& viewer) {
    registerWorldPartitionComponents();
    drainJobs();
    cells_.clear();
    tracked_.clear();
    rejected_.clear();
    removeCache();
    frame_ = 0;
    rescans_ = 0;
    stats_ = PartitionStats{};
    active_ = false;
    origin_ = world.origin();
    scene_generation_ = world.sceneGeneration();
    raw_layer_ids_.clear();
    deps_.clear();
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
    // Capas: como dice el componente y, encima, las que ya se tocaron
    // (setDataLayerActive antes de empezar o en una escena anterior).
    layer_state_.clear();
    for (const DataLayer& l : settings_.data_layers) {
        const std::string n = trim(l.name);
        if (!n.empty()) layer_state_[n] = l.active;
    }
    for (const auto& [n, a] : layer_overrides_) layer_state_[n] = a;
    // Dependencias (rejillas por nombre).
    for (const CellDependency& d : settings_.dependencies) {
        const int from = findGrid(settings_, d.grid);
        const int to = findGrid(settings_, d.load_grid);
        if (from < 0 || to < 0) {
            std::cerr << "[WorldPartition] Dependencia con una rejilla que no existe (\"" << d.grid << "\" -> \""
                      << d.load_grid << "\"): no se usa\n";
            continue;
        }
        deps_[GridCoord{from, d.x, d.z}].push_back(GridCoord{to, d.load_x, d.load_z});
    }
    active_ = true;
    // Raices que se pueden descargar, por celda, y las celdas en disco.
    rescan(world);
    registerExternalCells(world);
    stats_.streamed_entities = static_cast<int>(tracked_.size());
    for (const auto& [coord, cell] : cells_) {
        if (!cell.loaded) stats_.streamed_entities += cell.entities;
    }
    // Lo cercano (y lo que pide), cargado ya (el jugador empieza con su
    // alrededor completo); lo lejano y lo de las capas apagadas, fuera
    // (memoria libre desde el primer frame).
    for (const auto& [key, d] : desires(sources(world, viewer))) {
        const auto it = cells_.find(key);
        if (it == cells_.end()) continue;
        Cell& cell = it->second;
        if (d.load && !cell.loaded) loadCellNow(world, cell);
        if (!d.load && cell.loaded) unloadCell(world, key, cell);
    }
    updateProxies(world);
    proxies_dirty_ = false;
    refreshStats();
    std::cout << "[WorldPartition] " << stats_.cells << " celdas de " << settings_.cell_size << " m";
    if (!settings_.grids.empty()) std::cout << " (y " << settings_.grids.size() << " rejillas mas)";
    std::cout << ", " << stats_.streamed_entities << " objetos; descargados " << stats_.unloaded_entities;
    if (stats_.external_cells > 0) std::cout << " (" << stats_.external_cells << " celdas en disco)";
    std::cout << "\n";
}

void WorldPartitionSystem::unloadCell(ecs::World& world, const CellKey& c, Cell& cell) {
    if (!cell.loaded || cell.loading()) return;
    std::vector<std::string> stored;
    for (const entt::entity h : cell.roots) {
        if (!world.valid(h)) {
            untrack(h);
            continue;
        }
        ecs::Entity r = world.wrap(h);
        // Se movio a otra celda (o le cambiaron la rejilla o las capas) o ya
        // es hijo de algo: el proximo repaso la pone en su sitio.
        if (r.parent().valid() || !(keyOf(r) == c)) continue;
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
    cell.stored.reset();
    cell.bytes = 0;
    cell.disk_bytes = 0;
    cell.file.clear();
    cell.origin = world.origin();  // las posiciones guardadas son relativas a este
    if (!stored.empty() && g_async_cells.get()) {
        // A disco en el hilo de IO. Mientras escribe, lo guardado sigue en
        // memoria (compartido, nadie lo cambia): volver a cargarla lo usa.
        auto texts = std::make_shared<const std::vector<std::string>>(std::move(stored));
        for (const std::string& text : *texts) cell.bytes += text.size();
        cell.stored = texts;
        auto w = std::make_shared<PendingWrite>();
        w->file = cacheFile(c);
        // Todas las anteriores (no solo la ultima: pollWrites suelta la ultima
        // aunque una mas vieja siga escribiendo cacheFile(c)).
        if (cell.write) cell.older_writes.push_back(std::move(cell.write));
        cell.older_writes.erase(std::remove_if(cell.older_writes.begin(), cell.older_writes.end(),
                                               [](const std::shared_ptr<PendingWrite>& p) { return !p || p->ready(); }),
                                cell.older_writes.end());
        if (!cell.older_writes.empty()) {
            // Otra escritura de esta celda aun en marcha: otro archivo (dos a
            // la vez romperian el rename en Windows).
            std::filesystem::path alt = w->file;
            alt.replace_extension();
            alt += "_" + std::to_string(++write_seq_) + kCellExtension;
            w->file = alt;
        }
        // Sin `this`: solo el estado de la escritura y los textos.
        w->job = jobs::scheduleIo(
            [w, texts, origin = cell.origin]() {
                std::size_t written = 0;
                const bool ok = writeCellFile(w->file, *texts, origin, &written);
                w->bytes = written;
                w->ok.store(ok, std::memory_order_release);
                w->done.store(true, std::memory_order_release);
            },
            jobs::Priority::Normal);
        io_jobs_.push_back(w->job);
        cell.write = std::move(w);
    } else if (!stored.empty()) {
        // A disco (la cache de la sesion); si no se puede, en memoria.
        const std::filesystem::path file = cacheFile(c);
        std::size_t written = 0;
        if (writeCellFile(file, stored, cell.origin, &written)) {
            cell.file = file;
            cell.disk_bytes = written;
        } else {
            for (const std::string& text : stored) cell.bytes += text.size();
            cell.stored = std::make_shared<const std::vector<std::string>>(std::move(stored));
        }
    }
    cell.loaded = false;
    proxies_dirty_ = true;
    ++stats_.unloads;
}

bool WorldPartitionSystem::requestLoad(Cell& cell) {
    if (cell.loaded || cell.loading()) return true;
    cell.pending.clear();
    cell.next = 0;
    const bool async = g_async_cells.get();
    if (cell.stored && async) {
        // En memoria (o escribiendose): se parsea en un hilo trabajador. Lo
        // guardado se queda hasta que termine (loadCellNow puede necesitarlo).
        auto read = std::make_shared<PendingRead>();
        cell.read = read;
        cell.reading = true;
        jobs::schedule(
            [read, texts = cell.stored]() {
                for (const std::string& text : *texts) {
                    if (jobs::isCancelled(read->cancel)) break;
                    if (auto parsed = ecs::parseEntities(text)) read->roots.push_back(PendingRoot{{}, std::move(parsed)});
                }
                read->done.store(true, std::memory_order_release);
            },
            nullptr, jobs::Priority::Normal);
    } else if (cell.stored) {
        for (const std::string& text : *cell.stored) cell.pending.push_back(PendingRoot{text, nullptr});
        cell.stored.reset();
        cell.bytes = 0;
    } else if (!cell.file.empty()) {
        if (async && readsInFlight() >= std::max(g_max_reads.get(), 1)) return false;  // el proximo frame
        auto read = std::make_shared<PendingRead>();
        cell.read = read;
        cell.reading = true;
        // Lectura en el hilo de IO y, con wp.AsyncCells, el parseo en un hilo
        // trabajador (el de IO sigue leyendo). Sin `this`.
        io_jobs_.push_back(jobs::scheduleIo(
            [read, file = cell.file, async]() {
                if (jobs::isCancelled(read->cancel)) {
                    read->done.store(true, std::memory_order_release);
                    return;
                }
                std::vector<std::string> texts = readCellFile(file);
                if (!async || texts.empty() || jobs::isCancelled(read->cancel)) {
                    for (std::string& text : texts) read->roots.push_back(PendingRoot{std::move(text), nullptr});
                    read->done.store(true, std::memory_order_release);
                    return;
                }
                auto shared = std::make_shared<const std::vector<std::string>>(std::move(texts));
                jobs::schedule(
                    [read, shared]() {
                        for (const std::string& text : *shared) {
                            if (jobs::isCancelled(read->cancel)) break;
                            if (auto parsed = ecs::parseEntities(text)) {
                                read->roots.push_back(PendingRoot{{}, std::move(parsed)});
                            }
                        }
                        read->done.store(true, std::memory_order_release);
                    },
                    nullptr, jobs::Priority::Normal);
            },
            jobs::Priority::Normal));
    }
    if (!cell.loading()) finishLoad(cell);  // vacia
    return true;
}

void WorldPartitionSystem::finishLoad(Cell& cell) {
    cell.loaded = true;
    cell.pending.clear();
    cell.pending.shrink_to_fit();
    cell.next = 0;
    cell.read.reset();
    cell.reading = false;
    cell.stored.reset();
    cell.bytes = 0;
    cell.file.clear();
    cell.disk_bytes = 0;
    proxies_dirty_ = true;
    ++stats_.loads;
}

void WorldPartitionSystem::pumpLoads(ecs::World& world, int budget) {
    for (auto& [coord, cell] : cells_) {
        if (cell.reading && cell.read && cell.read->ready()) {
            cell.pending = std::move(cell.read->roots);
            cell.read.reset();
            cell.next = 0;
            cell.reading = false;
            // Parseado lo que habia en memoria: ya no hace falta.
            cell.stored.reset();
            cell.bytes = 0;
            if (cell.pending.empty()) finishLoad(cell);
        }
    }
    // Ademas del limite de objetos, uno de tiempo (los que ya vienen parseados).
    const bool async = g_async_cells.get();
    jobs::FrameBudget time_budget(g_restore_budget_ms.get());
    // track() puede crear celdas: el map no mueve las que ya hay.
    for (auto& [coord, cell] : cells_) {
        if (cell.reading || cell.next >= cell.pending.size()) continue;
        while (budget > 0 && cell.next < cell.pending.size()) {
            if (async && !time_budget.spend()) break;
            const PendingRoot root = std::move(cell.pending[cell.next++]);
            const ecs::Entity r = restorePending(world, root, cell.origin);
            if (r.valid() && !r.parent().valid()) track(r.handle(), keyOf(r));
            --budget;
        }
        if (cell.next >= cell.pending.size()) finishLoad(cell);
        if (budget <= 0 || (async && !time_budget.hasTime())) break;
    }
}

void WorldPartitionSystem::loadCellNow(ecs::World& world, Cell& cell) {
    if (cell.loaded) return;
    if (cell.reading && cell.read && cell.read->ready()) {
        cell.pending = std::move(cell.read->roots);
        cell.read.reset();
        cell.next = 0;
        cell.reading = false;
        cell.stored.reset();
        cell.bytes = 0;
    }
    if (cell.reading) {
        // Aun en otro hilo: se cancela y se lee aqui (esperando al disco).
        if (cell.read) cell.read->cancel->cancelled.store(true, std::memory_order_release);
        cell.read.reset();
        cell.reading = false;
        cell.pending.clear();
        cell.next = 0;
        if (cell.stored) {
            for (const std::string& text : *cell.stored) cell.pending.push_back(PendingRoot{text, nullptr});
            cell.stored.reset();
            cell.bytes = 0;
        } else if (!cell.file.empty()) {
            for (std::string& text : readCellFile(cell.file)) cell.pending.push_back(PendingRoot{std::move(text), nullptr});
        }
    } else if (!cell.loading()) {
        // Sincrono: lo de memoria o el archivo, aqui.
        cell.pending.clear();
        cell.next = 0;
        if (cell.stored) {
            for (const std::string& text : *cell.stored) cell.pending.push_back(PendingRoot{text, nullptr});
            cell.stored.reset();
            cell.bytes = 0;
        } else if (!cell.file.empty()) {
            for (std::string& text : readCellFile(cell.file)) cell.pending.push_back(PendingRoot{std::move(text), nullptr});
        }
    }
    while (cell.next < cell.pending.size()) {
        const PendingRoot root = std::move(cell.pending[cell.next++]);
        const ecs::Entity r = restorePending(world, root, cell.origin);
        if (r.valid() && !r.parent().valid()) track(r.handle(), keyOf(r));
    }
    finishLoad(cell);
}

void WorldPartitionSystem::update(ecs::World& world, const Vec3& viewer) {
    if (!active_) return;
    if (world.sceneGeneration() != scene_generation_) {
        // Otra escena sin end(): lecturas canceladas, celdas olvidadas (sin
        // restaurar nada: sus objetos eran de la escena anterior).
        end(world, /*restore=*/false);
        return;
    }
    origin_ = world.origin();
    ++frame_;
    // Escrituras de celdas terminadas: lo guardado sale de la memoria.
    pollWrites();
    // Lo creado, borrado o movido en Play: cada pocos frames y solo las raices
    // (antes, el arbol entero con cada hijo en cada frame).
    const bool rescanned = frame_ % 10 == 0;
    if (rescanned) rescan(world);
    // Lo que entra (las mas cercanas primero) y lo que sale (las mas lejanas
    // y las de capas apagadas primero), cada cosa con su presupuesto por
    // frame. desires() va en el orden de cells_ (y nada cambia entre medias).
    std::vector<std::pair<float, CellKey>> to_load;
    std::vector<std::pair<float, CellKey>> to_unload;
    {
        const auto wanted = desires(sources(world, viewer));
        auto cell_it = cells_.begin();
        for (const auto& [key, d] : wanted) {
            const Cell& cell = cell_it->second;
            ++cell_it;
            if (!cell.loaded && !cell.loading() && d.load) to_load.emplace_back(d.priority, key);
            if (cell.loaded && !d.keep) to_unload.emplace_back(d.priority, key);
        }
    }
    if (!to_unload.empty()) {
        // Antes de guardar: que lo que se movio a estas celdas tambien salga.
        if (!rescanned) rescan(world);
        std::sort(to_unload.begin(), to_unload.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        const int budget = std::max(settings_.unloads_per_frame, 1);
        for (int i = 0; i < static_cast<int>(to_unload.size()) && i < budget; ++i) {
            const CellKey c = to_unload[static_cast<std::size_t>(i)].second;
            unloadCell(world, c, cells_[c]);
        }
    }
    std::sort(to_load.begin(), to_load.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const int budget = std::max(settings_.loads_per_frame, 1);
    for (int i = 0; i < static_cast<int>(to_load.size()) && i < budget; ++i) {
        // false: demasiadas lecturas en marcha (las mas cercanas ya van).
        if (!requestLoad(cells_[to_load[static_cast<std::size_t>(i)].second])) break;
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
        // El HLOD es de lo de la rejilla principal sin capas (Construir HLOD).
        const auto it = cells_.find(CellKey{0, p.cell_x, p.cell_z, 0});
        const bool show = active_ && it != cells_.end() && !it->second.loaded;
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
    origin_ = world.origin();
    for (auto& [coord, cell] : cells_) loadCellNow(world, cell);
    if (proxies_dirty_) {
        updateProxies(world);
        proxies_dirty_ = false;
    }
    refreshStats();
}

void WorldPartitionSystem::end(ecs::World& world, bool restore) {
    if (active_ && restore) loadAll(world);
    // La cache no se borra con escrituras o lecturas en marcha.
    drainJobs();
    cells_.clear();
    tracked_.clear();
    rejected_.clear();
    removeCache();
    deps_.clear();
    layer_state_.clear();
    layer_overrides_.clear();
    raw_layer_ids_.clear();
    active_ = false;
    stats_ = PartitionStats{};
}

bool WorldPartitionSystem::cellLoaded(const CellCoord& c) const { return cellLoaded(0, c); }

bool WorldPartitionSystem::cellLoaded(int grid, const CellCoord& c) const {
    // Las capas de un mismo (rejilla, x, z) van seguidas.
    bool any = false;
    for (auto it = cells_.lower_bound(CellKey{grid, c.x, c.z, 0});
         it != cells_.end() && it->first.grid == grid && it->first.x == c.x && it->first.z == c.z; ++it) {
        if (it->second.loaded) return true;
        any = true;
    }
    return !any;
}

std::vector<std::pair<CellCoord, bool>> WorldPartitionSystem::cells() const {
    std::map<CellCoord, bool> primary;
    for (const auto& [key, cell] : cells_) {
        if (key.grid != 0) continue;
        bool& loaded = primary.emplace(CellCoord{key.x, key.z}, false).first->second;
        loaded = loaded || cell.loaded;
    }
    return {primary.begin(), primary.end()};
}

std::vector<CellInfo> WorldPartitionSystem::cellInfos() const {
    std::vector<CellInfo> out;
    out.reserve(cells_.size());
    for (const auto& [key, cell] : cells_) {
        CellInfo info;
        info.grid = key.grid;
        info.coord = CellCoord{key.x, key.z};
        info.cell_size = gridCellSize(settings_, key.grid);
        if (key.layers > 0 && key.layers < static_cast<int>(layer_sets_.size())) {
            info.layers = layer_sets_[static_cast<std::size_t>(key.layers)];
        }
        info.loaded = cell.loaded;
        info.loading = cell.loading();
        out.push_back(std::move(info));
    }
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
    stats_.reads_in_flight = 0;
    stats_.writes_in_flight = 0;
    for (const auto& [coord, cell] : cells_) {
        if (cell.reading) ++stats_.reads_in_flight;
        if (cell.write && !cell.write->ready()) ++stats_.writes_in_flight;
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
