#include "CramionCore/ai/Crowd.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/navigation/Navigation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace cramion::ai {

using core::Vec3;

namespace {
CrowdSystem* g_active = nullptr;

void setVisibleRecursive(ecs::Entity e, bool visible, int depth = 0) {
    if (depth > 32) return;
    if (ecs::MeshRenderer* mr = e.tryGet<ecs::MeshRenderer>()) mr->visible = visible;
    for (const entt::entity c : e.children()) setVisibleRecursive(e.world()->wrap(c), visible, depth + 1);
}
}  // namespace

CrowdSystem* activeCrowds() { return g_active; }
void setActiveCrowds(CrowdSystem* system) { g_active = system; }

void CrowdSpawner::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 4> kBehaviors = {"Deambular", "Recorrer puntos (hijos)", "Seguir objetivo",
                                                              "Huir"};
    v.asset({"prefab", "Prefab", "Lo que se crea (personaje con su Animator)"}, prefab, assets::AssetType::Prefab);
    v.field({"count", "Cantidad"}, count, 1, 5000);
    v.field({"radius", "Radio"}, radius, ecs::FloatRange{1.0f, 2000.0f, 0.5f, "%.1f m"});
    ecs::enumField(v, {"behavior", "Comportamiento"}, behavior, kBehaviors);
    v.field({"speed_min", "Velocidad minima"}, speed_min, ecs::FloatRange{0.1f, 20.0f, 0.05f, "%.2f m/s"});
    v.field({"speed_max", "Velocidad maxima"}, speed_max, ecs::FloatRange{0.1f, 20.0f, 0.05f, "%.2f m/s"});
    v.field({"wait_min", "Espera minima"}, wait_min, ecs::FloatRange{0.0f, 60.0f, 0.05f, "%.2f s"});
    v.field({"wait_max", "Espera maxima"}, wait_max, ecs::FloatRange{0.0f, 60.0f, 0.05f, "%.2f s"});
    v.entity({"target", "Objetivo", "Seguir / Huir (vacio en Huir = el que tenga el tag Player)"}, target);
    v.field({"flee_radius", "Distancia de susto"}, flee_radius, ecs::FloatRange{0.5f, 200.0f, 0.1f, "%.1f m"});
    v.field({"random_waypoints", "Puntos al azar"}, random_waypoints);
    v.field({"cull_distance", "Distancia de culling", "Mas lejos de la camara: ocultos y sin decidir (0 = nunca)"},
            cull_distance, ecs::FloatRange{0.0f, 5000.0f, 1.0f, "%.0f m"});
    v.field({"decisions_per_frame", "Decisiones por frame"}, decisions_per_frame, 1, 2000);
    v.field({"seed", "Semilla"}, seed, 0, 1000000);
    v.field({"spawn_on_start", "Crear al empezar"}, spawn_on_start);
}

void CrowdAgent::reflect(ecs::PropertyVisitor& v) {
    v.entity({"spawner", "Spawner"}, spawner);
    v.field({"waypoint", "Punto actual"}, waypoint, 0, 10000);
}

void registerCrowdComponents() {
    auto& r = ecs::ComponentRegistry::instance();
    if (r.find("CrowdSpawner") != nullptr) return;
    r.registerComponent<CrowdSpawner>("CrowdSpawner", "Multitud (Crowd Spawner)", "IA");
    r.registerComponent<CrowdAgent>("CrowdAgent", "Agente de multitud", "IA");
}

float CrowdSystem::next_rand() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
}

void CrowdSystem::begin(ecs::World& world) {
    registerCrowdComponents();
    stats_ = CrowdStats{};
    std::vector<ecs::Entity> spawners;
    for (const entt::entity h : world.registry().view<CrowdSpawner>()) {
        const ecs::Entity e = world.wrap(h);
        if (e.activeInHierarchy() && e.get<CrowdSpawner>().spawn_on_start) spawners.push_back(e);
    }
    for (ecs::Entity& s : spawners) spawn(world, s);
}

int CrowdSystem::spawn(ecs::World& world, ecs::Entity spawner_entity) {
    if (!spawner_entity.valid() || !spawner_entity.has<CrowdSpawner>()) return 0;
    despawn(world, spawner_entity);
    const CrowdSpawner s = spawner_entity.get<CrowdSpawner>();
    if (!s.prefab.valid() || !resolver_) return 0;
    const std::filesystem::path file = resolver_(s.prefab.uuid);
    if (file.empty()) {
        std::cerr << "[Multitud] No se encuentra el prefab de " << spawner_entity.name() << "\n";
        return 0;
    }
    rng_ = static_cast<std::uint32_t>(s.seed) * 2654435761u + 1u;
    const Vec3 center = spawner_entity.worldPosition();
    int created = 0;
    for (int i = 0; i < s.count; ++i) {
        Vec3 p{};
        bool placed = nav_ != nullptr && nav_->randomPoint(center, s.radius, p);
        if (!placed) {
            const float a = next_rand() * 6.2831853f;
            const float r = std::sqrt(next_rand()) * s.radius;
            p = center + Vec3{std::cos(a) * r, 0.0f, std::sin(a) * r};
        }
        ecs::Entity e = ecs::instantiatePrefab(world, file);
        if (!e.valid()) break;
        e.setWorldPosition(p);
        e.setLocalEulerDegrees(Vec3{0.0f, next_rand() * 360.0f, 0.0f});
        navigation::NavAgent& agent =
            e.has<navigation::NavAgent>() ? e.get<navigation::NavAgent>() : e.add<navigation::NavAgent>();
        agent.speed = s.speed_min + (std::max(s.speed_max, s.speed_min) - s.speed_min) * next_rand();
        CrowdAgent& ca = e.add<CrowdAgent>();
        ca.spawner = spawner_entity.uuid();
        ca.base_speed = agent.speed;
        ca.wait = next_rand() * s.wait_max;  // no salen todos a la vez
        ca.waypoint = s.random_waypoints ? static_cast<int>(next_rand() * 1000.0f) : 0;
        ++created;
    }
    std::cout << "[Multitud] " << spawner_entity.name() << ": " << created << " agentes\n";
    return created;
}

void CrowdSystem::despawn(ecs::World& world, ecs::Entity spawner_entity) {
    if (!spawner_entity.valid()) return;
    std::vector<ecs::Entity> gone;
    for (const entt::entity h : world.registry().view<CrowdAgent>()) {
        if (world.registry().get<CrowdAgent>(h).spawner == spawner_entity.uuid()) gone.push_back(world.wrap(h));
    }
    for (ecs::Entity& e : gone) world.destroy(e);
}

void CrowdSystem::end(ecs::World& world) {
    std::vector<ecs::Entity> gone;
    for (const entt::entity h : world.registry().view<CrowdAgent>()) gone.push_back(world.wrap(h));
    for (ecs::Entity& e : gone) world.destroy(e);
    stats_ = CrowdStats{};
}

void CrowdSystem::decide(ecs::World& world, ecs::Entity agent, CrowdAgent& state, const CrowdSpawner& s,
                         ecs::Entity spawner_entity, const Vec3& threat, bool has_threat) {
    if (nav_ == nullptr) return;
    navigation::NavAgent* na = agent.tryGet<navigation::NavAgent>();
    const Vec3 pos = agent.worldPosition();
    // Huir: si la amenaza esta cerca, lejos de ella y mas deprisa.
    if (s.behavior == CrowdBehavior::Flee && has_threat) {
        Vec3 away = pos - threat;
        away.y = 0.0f;
        const float d = core::length(away);
        if (d < s.flee_radius) {
            const Vec3 dir = d > 1e-3f ? away * (1.0f / d) : Vec3{1.0f, 0.0f, 0.0f};
            Vec3 goal = pos + dir * (s.flee_radius * 1.5f);
            Vec3 on_mesh{};
            if (nav_->projectPoint(goal, on_mesh, 4.0f)) goal = on_mesh;
            if (na != nullptr) na->speed = state.base_speed * 2.2f;
            state.fleeing = true;
            nav_->moveTo(agent, goal);
            state.wait = 0.0f;
            return;
        }
        if (state.fleeing && na != nullptr) na->speed = state.base_speed;
        state.fleeing = false;
    }
    if (nav_->isMoving(agent)) return;
    if (state.wait > 0.0f) return;
    Vec3 goal = pos;
    bool have_goal = false;
    switch (s.behavior) {
        case CrowdBehavior::Waypoints: {
            const auto& kids = spawner_entity.children();
            if (!kids.empty()) {
                const int n = static_cast<int>(kids.size());
                state.waypoint = s.random_waypoints ? static_cast<int>(next_rand() * static_cast<float>(n)) % n
                                                    : (state.waypoint + 1) % n;
                goal = world.wrap(kids[static_cast<std::size_t>(state.waypoint)]).worldPosition();
                have_goal = true;
            }
            break;
        }
        case CrowdBehavior::Follow: {
            const ecs::Entity t = s.target.valid() ? world.find(s.target) : ecs::Entity{};
            if (t.valid()) {
                const float a = next_rand() * 6.2831853f;
                const float r = 1.5f + next_rand() * 3.0f;
                goal = t.worldPosition() + Vec3{std::cos(a) * r, 0.0f, std::sin(a) * r};
                have_goal = true;
            }
            break;
        }
        case CrowdBehavior::Wander:
        case CrowdBehavior::Flee:
            have_goal = nav_->randomPoint(spawner_entity.worldPosition(), s.radius, goal);
            break;
    }
    if (have_goal) nav_->moveTo(agent, goal);
    state.wait = s.behavior == CrowdBehavior::Follow ? 1.0f
                                                     : s.wait_min + (std::max(s.wait_max, s.wait_min) - s.wait_min) * next_rand();
}

void CrowdSystem::update(ecs::World& world, float dt, const Vec3& viewer) {
    stats_.agents = 0;
    stats_.visible = 0;
    stats_.decisions = 0;
    stats_.spawners = static_cast<int>(world.registry().view<CrowdSpawner>().size());
    auto view = world.registry().view<CrowdAgent>();
    std::vector<entt::entity> agents(view.begin(), view.end());
    stats_.agents = static_cast<int>(agents.size());
    if (agents.empty()) return;
    // Amenazas por spawner (Huir) y el reparto de decisiones.
    struct SpawnerInfo {
        ecs::Entity entity;
        const CrowdSpawner* spawner = nullptr;
        Vec3 threat{};
        bool has_threat = false;
        int budget = 0;
    };
    std::vector<std::pair<Uuid, SpawnerInfo>> infos;
    const auto info_of = [&](const Uuid& id) -> SpawnerInfo* {
        for (auto& [k, v] : infos) {
            if (k == id) return &v;
        }
        SpawnerInfo si;
        si.entity = world.find(id);
        if (!si.entity.valid() || !si.entity.has<CrowdSpawner>()) return nullptr;
        si.spawner = &si.entity.get<CrowdSpawner>();
        si.budget = si.spawner->decisions_per_frame;
        if (si.spawner->behavior == CrowdBehavior::Flee) {
            ecs::Entity t = si.spawner->target.valid() ? world.find(si.spawner->target) : world.findWithTag("Player");
            if (t.valid()) {
                si.threat = t.worldPosition();
                si.has_threat = true;
            }
        }
        infos.emplace_back(id, si);
        return &infos.back().second;
    };
    const std::size_t n = agents.size();
    cursor_ %= n;
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t idx = (cursor_ + k) % n;
        ecs::Entity e = world.wrap(agents[idx]);
        CrowdAgent& ca = e.get<CrowdAgent>();
        SpawnerInfo* si = info_of(ca.spawner);
        if (si == nullptr) continue;
        const CrowdSpawner& s = *si->spawner;
        // Culling por distancia.
        const Vec3 d = e.worldPosition() - viewer;
        const bool culled = s.cull_distance > 0.0f && core::dot(d, d) > s.cull_distance * s.cull_distance;
        if (culled != ca.hidden) {
            setVisibleRecursive(e, !culled);
            ca.hidden = culled;
        }
        if (culled) continue;
        ++stats_.visible;
        if (ca.wait > 0.0f && (nav_ == nullptr || !nav_->isMoving(e))) ca.wait -= dt;
        if (si->budget <= 0) continue;
        --si->budget;
        ++stats_.decisions;
        decide(world, e, ca, s, si->entity, si->threat, si->has_threat);
    }
    cursor_ = (cursor_ + static_cast<std::size_t>(std::max(stats_.decisions, 1))) % n;
}

}  // namespace cramion::ai
