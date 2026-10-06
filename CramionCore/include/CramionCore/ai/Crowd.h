#ifndef CRAMION_CORE_AI_CROWD_H
#define CRAMION_CORE_AI_CROWD_H

// Multitudes (como Mass AI de Unreal / los crowd systems de Assassin's Creed):
// un CrowdSpawner crea en Play muchas copias de un prefab en puntos al azar de
// la navmesh y las mueve con NavAgent (DetourCrowd: se esquivan entre si)
// con un comportamiento:
//
//   Deambular     van a puntos al azar dentro del radio y esperan un poco.
//   Puntos        recorren las entidades hijas del spawner (en orden o al azar).
//   Seguir        van hacia el objetivo (otra entidad) y se quedan alrededor.
//   Huir          deambulan, y huyen del objetivo (o del "Player") si se acerca.
//
// Escala: las decisiones se reparten entre frames (presupuesto por frame), y
// los agentes mas alla de la distancia de culling se ocultan y no deciden
// (la animacion ya baja de frecuencia con la distancia: anim.lod.*).

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/AssetTypes.h"
#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cramion::ecs {
class World;
class Entity;
}  // namespace cramion::ecs

namespace cramion::navigation {
class NavigationSystem;
}

namespace cramion::ai {

enum class CrowdBehavior : int { Wander = 0, Waypoints = 1, Follow = 2, Flee = 3 };

struct CrowdSpawner {
    assets::AssetRef prefab{{}, assets::AssetType::Prefab};
    int count = 50;
    float radius = 30.0f;           // donde aparecen y deambulan (m)
    CrowdBehavior behavior = CrowdBehavior::Wander;
    float speed_min = 1.2f;         // m/s (cada uno, al azar entre min y max)
    float speed_max = 2.0f;
    float wait_min = 0.5f;          // s de espera al llegar
    float wait_max = 4.0f;
    Uuid target;                    // Seguir / Huir (vacio en Huir = el "Player")
    float flee_radius = 8.0f;       // Huir: a que distancia se asustan
    bool random_waypoints = false;  // Puntos: en orden o al azar
    float cull_distance = 120.0f;   // mas lejos: ocultos y sin decidir (0 = nunca)
    int decisions_per_frame = 64;
    int seed = 1;
    bool spawn_on_start = true;

    void reflect(ecs::PropertyVisitor& v);
};

// En cada agente creado (no se guarda en la escena: lo crea el sistema).
struct CrowdAgent {
    Uuid spawner;
    float wait = 0.0f;
    int waypoint = 0;
    bool hidden = false;
    bool fleeing = false;
    float base_speed = 0.0f;
    void reflect(ecs::PropertyVisitor& v);
};

void registerCrowdComponents();

struct CrowdStats {
    int spawners = 0;
    int agents = 0;
    int visible = 0;
    int decisions = 0;  // este frame
};

class CrowdSystem {
public:
    // Ruta del .crprefab de un UUID (la base de datos de assets).
    using PrefabResolver = std::function<std::filesystem::path(const Uuid&)>;
    void setPrefabResolver(PrefabResolver resolver) { resolver_ = std::move(resolver); }
    void setNavigation(navigation::NavigationSystem* nav) { nav_ = nav; }

    // Al empezar Play / el juego: crea las multitudes con spawn_on_start.
    void begin(ecs::World& world);
    void update(ecs::World& world, float dt, const core::Vec3& viewer);
    void end(ecs::World& world);
    // Crea (o recrea) la multitud de un spawner. Devuelve cuantos creo.
    int spawn(ecs::World& world, ecs::Entity spawner);
    void despawn(ecs::World& world, ecs::Entity spawner);
    const CrowdStats& stats() const { return stats_; }

private:
    void decide(ecs::World& world, ecs::Entity agent, CrowdAgent& state, const CrowdSpawner& spawner,
                ecs::Entity spawner_entity, const core::Vec3& threat, bool has_threat);
    PrefabResolver resolver_;
    navigation::NavigationSystem* nav_ = nullptr;
    std::uint32_t rng_ = 12345u;
    std::size_t cursor_ = 0;  // por donde van las decisiones (reparto entre frames)
    CrowdStats stats_{};
    float next_rand();
};

CrowdSystem* activeCrowds();
void setActiveCrowds(CrowdSystem* system);

}  // namespace cramion::ai

#endif  // CRAMION_CORE_AI_CROWD_H
