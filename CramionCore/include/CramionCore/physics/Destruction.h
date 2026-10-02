#ifndef CRAMION_CORE_PHYSICS_DESTRUCTION_H
#define CRAMION_CORE_PHYSICS_DESTRUCTION_H

// Objetos que se rompen (como los Geometry Collection de Unreal): el
// componente Destructible usa un asset .crfracture (Fracture.h) con los
// trozos ya calculados. Al recibir golpes pierde vida; al llegar a 0 (o al
// llamar a fracture() desde un script) el objeto se oculta y aparecen sus
// trozos como cuerpos rigidos, empujados desde el punto del golpe. Los trozos
// grandes con hijos (fractura de varios niveles) se pueden volver a romper.
// Los escombros duran `debris_lifetime` segundos, encogen y desaparecen; hay
// un maximo de trozos vivos en toda la escena.

#include "CramionCore/asset/AssetTypes.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/Fracture.h"

#include <CramionFX/core/Math.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::assets {
class AssetManager;
}

namespace cramion::physics {

class PhysicsSystem;

struct Destructible {
    assets::AssetRef fracture{{}, assets::AssetType::Fracture};
    // Vida y golpes
    float health = 100.0f;
    bool break_on_impact = true;
    float damage_threshold = 3.0f;    // m/s de choque que no hacen dano
    float damage_per_speed = 20.0f;   // dano por cada m/s por encima del umbral
    float break_force = 4.0f;         // m/s con que salen los trozos desde el golpe
    // Trozos
    assets::AssetRef interior_material{{}, assets::AssetType::Material};  // .crmat del corte (vacio = color)
    core::Vec3 interior_color{0.55f, 0.5f, 0.45f};  // sRGB
    float density = 600.0f;           // kg/m3 si el objeto no tiene Rigidbody
    bool multi_level = true;          // los trozos con hijos se rompen otra vez
    float piece_health = 40.0f;       // vida de esos trozos
    // Escombros
    float debris_lifetime = 10.0f;    // s (0 = no desaparecen)
    float fade_time = 1.5f;           // s encogiendo antes de desaparecer
    int max_active_pieces = 300;      // en toda la escena (los mas viejos se van)

    void reflect(ecs::PropertyVisitor& v);
};

void registerDestructionComponents();

struct BreakEvent {
    ecs::Entity entity;             // el objeto (o trozo) que se rompio (ya inactivo)
    core::Vec3 point{};
    float force = 0.0f;
    std::vector<ecs::Entity> pieces;
};

class DestructionSystem {
public:
    void setAssetManager(assets::AssetManager* manager) { assets_ = manager; }

    // Al empezar/parar la simulacion: olvida los trozos (la escena se restaura).
    void reset();
    // Cada paso de fisica del frame: golpes, roturas pendientes y escombros.
    void update(ecs::World& world, PhysicsSystem& physics, float delta_seconds);

    // Rompe ya (en el proximo update) el objeto con Destructible (o un trozo
    // rompible). `force`: m/s extra hacia fuera desde `point`.
    bool fracture(ecs::Entity entity, const core::Vec3& point, float force = -1.0f);
    // Quita vida (rompe si llega a 0).
    void applyDamage(ecs::Entity entity, float damage, const core::Vec3& point, float force = -1.0f);
    float health(ecs::Entity entity) const;
    bool isBroken(ecs::Entity entity) const;

    int activePieces() const { return static_cast<int>(debris_.size()); }
    // Avisos de rotura (scripts: OnBreak; editor; sonido y particulas).
    using BreakListener = std::function<void(const BreakEvent&)>;
    int addBreakListener(BreakListener listener);
    void removeBreakListener(int id);

    // Datos de un .crfracture (cacheados).
    std::shared_ptr<const FractureData> load(const Uuid& uuid);

private:
    struct Breakable {          // un trozo que puede romperse otra vez
        std::shared_ptr<const FractureData> data;
        int piece = -1;
        float health = 0.0f;
        Destructible settings;  // del objeto original
    };
    struct Debris {
        entt::entity handle = entt::null;
        float age = 0.0f;
        float lifetime = 0.0f;
        float fade = 1.0f;
        core::Vec3 scale{1.0f, 1.0f, 1.0f};
    };
    struct Request {
        entt::entity handle = entt::null;
        core::Vec3 point{};
        float force = -1.0f;
    };

    void breakEntity(ecs::World& world, PhysicsSystem& physics, ecs::Entity entity, const core::Vec3& point,
                     float force);
    void spawnPieces(ecs::World& world, PhysicsSystem& physics, ecs::Entity source, const FractureData& data,
                     const std::vector<int>& pieces, const core::Mat4& frame, const core::Vec3& velocity, float mass,
                     const Destructible& settings, const core::Vec3& point, float force,
                     std::vector<ecs::Entity>& spawned);

    assets::AssetManager* assets_ = nullptr;
    std::unordered_map<Uuid, std::shared_ptr<const FractureData>> cache_;
    std::unordered_map<entt::entity, Breakable> breakables_;
    std::unordered_map<entt::entity, float> health_;  // vida actual de los Destructible
    std::unordered_map<entt::entity, bool> broken_;
    std::deque<Debris> debris_;
    std::vector<Request> requests_;
    std::vector<std::pair<int, BreakListener>> listeners_;
    int next_listener_ = 1;
};

}  // namespace cramion::physics

#endif  // CRAMION_CORE_PHYSICS_DESTRUCTION_H
