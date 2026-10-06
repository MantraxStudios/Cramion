#include "CramionCore/world/WorldPartition.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/environment/Environment.h"
#include "CramionCore/foliage/Foliage.h"
#include "CramionCore/net/NetworkObject.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/ui/UI.h"
#include "CramionCore/water/Water.h"
#include "CramionCore/xr/XrRig.h"

#include <algorithm>
#include <cmath>
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
        e.has<WorldPartition>() || e.has<foliage::Foliage>() || e.has<foliage::Grass>() || e.has<HlodProxy>()) {
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

void WorldPartitionSystem::begin(ecs::World& world, const Vec3& viewer) {
    registerWorldPartitionComponents();
    cells_.clear();
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
    if (!found) return;
    active_ = true;
    // Raices que se pueden descargar, por celda.
    std::vector<ecs::Entity> roots;
    world.forEachDepthFirst([&](ecs::Entity e) {
        if (!e.parent().valid() && streamable(e)) roots.push_back(e);
    });
    for (const ecs::Entity& r : roots) {
        Cell& cell = cells_[cellOf(r.worldPosition())];
        ++cell.entities;
        ++stats_.streamed_entities;
    }
    // Descarga ya lo que queda lejos (memoria libre desde el primer frame).
    const auto srcs = sources(world, viewer);
    for (auto& [coord, cell] : cells_) {
        bool in_range = false;
        for (const auto& [pos, range] : srcs) in_range = in_range || distanceToCell(pos, coord) <= range;
        if (!in_range) unloadCell(world, coord, cell);
    }
    updateProxies(world);
    refreshStats();
    std::cout << "[WorldPartition] " << stats_.cells << " celdas de " << settings_.cell_size << " m, "
              << stats_.streamed_entities << " objetos; descargados " << stats_.unloaded_entities << "\n";
}

void WorldPartitionSystem::unloadCell(ecs::World& world, const CellCoord& c, Cell& cell) {
    if (!cell.loaded) return;
    std::vector<ecs::Entity> roots;
    world.forEachDepthFirst([&](ecs::Entity e) {
        if (!e.parent().valid() && streamable(e) && cellOf(e.worldPosition()) == c) roots.push_back(e);
    });
    cell.stored.clear();
    cell.bytes = 0;
    for (ecs::Entity& r : roots) {
        std::string text = ecs::serializeEntity(world, r);
        cell.bytes += text.size();
        cell.stored.push_back(std::move(text));
        world.destroy(r);
    }
    cell.entities = static_cast<int>(roots.size());
    cell.loaded = false;
    ++stats_.unloads;
}

void WorldPartitionSystem::loadCell(ecs::World& world, Cell& cell) {
    if (cell.loaded) return;
    for (const std::string& text : cell.stored) ecs::restoreEntities(world, text);
    cell.stored.clear();
    cell.bytes = 0;
    cell.loaded = true;
    ++stats_.loads;
}

void WorldPartitionSystem::update(ecs::World& world, const Vec3& viewer) {
    if (!active_) return;
    const auto srcs = sources(world, viewer);
    // Lo que entra: las mas cercanas primero, con presupuesto por frame.
    std::vector<std::pair<float, CellCoord>> to_load;
    for (auto& [coord, cell] : cells_) {
        float best = 1e30f;
        float range = settings_.load_range;
        for (const auto& [pos, r] : srcs) {
            const float d = distanceToCell(pos, coord) - r;
            if (d < best) {
                best = d;
                range = r;
            }
        }
        (void)range;
        if (!cell.loaded && best <= 0.0f) to_load.emplace_back(best, coord);
        if (cell.loaded && best > settings_.unload_margin) unloadCell(world, coord, cell);
    }
    std::sort(to_load.begin(), to_load.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const int budget = std::max(settings_.loads_per_frame, 1);
    for (int i = 0; i < static_cast<int>(to_load.size()) && i < budget; ++i) loadCell(world, cells_[to_load[i].second]);
    updateProxies(world);
    // Objetos que se movieron a celdas nuevas (lo que se mueve en Play).
    world.forEachDepthFirst([&](ecs::Entity e) {
        if (e.parent().valid() || !streamable(e)) return;
        const CellCoord c = cellOf(e.worldPosition());
        if (cells_.find(c) == cells_.end()) cells_[c].entities = 1;
    });
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
    for (auto& [coord, cell] : cells_) loadCell(world, cell);
    refreshStats();
}

void WorldPartitionSystem::end(ecs::World& world, bool restore) {
    if (active_ && restore) loadAll(world);
    cells_.clear();
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
    for (const auto& [coord, cell] : cells_) {
        if (cell.loaded) {
            ++stats_.loaded_cells;
        } else {
            stats_.unloaded_entities += static_cast<int>(cell.stored.size());
            stats_.stored_bytes += cell.bytes;
        }
    }
}

}  // namespace cramion::worldpart
