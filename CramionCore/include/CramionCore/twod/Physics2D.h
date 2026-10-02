#ifndef CRAMION_CORE_TWOD_PHYSICS2D_H
#define CRAMION_CORE_TWOD_PHYSICS2D_H

// Fisica 2D (como la de Unity con Box2D): cuerpos en el plano XY del mundo,
// que no chocan con la fisica 3D (Jolt). Motor propio de impulsos
// secuenciales: pares por barrido en X, contactos por SAT con recorte de
// caras (cajas y poligonos convexos) y circulos, friccion, rebote, arranque
// en caliente de los contactos, triggers y plataformas de un sentido.
//
//   Rigidbody2D        Dinamico (lo mueve la fisica), Cinematico (lo mueve su
//                      Transform o un script) o Estatico. Sin Rigidbody2D, los
//                      colliders son estaticos.
//   BoxCollider2D, CircleCollider2D, PolygonCollider2D
//                      formas (varias en la entidad = un cuerpo compuesto).
//   TilemapCollider2D  las celdas del Tilemap (Tilemap2D.h).
//   CompositeCollider2D  junta en rectangulos grandes los colliders de caja y
//                      de tilemap de su entidad (sin costuras al deslizar).
//
// Eventos para los scripts: OnCollisionEnter2D/Stay2D/Exit2D y
// OnTriggerEnter2D/Stay2D/Exit2D (other, contact).

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cramion::twod {

class TilesetLibrary;

enum class BodyType2D : int { Dynamic = 0, Kinematic = 1, Static = 2 };

struct Rigidbody2D {
    BodyType2D type = BodyType2D::Dynamic;
    float mass = 1.0f;
    float linear_drag = 0.0f;
    float angular_drag = 0.05f;
    float gravity_scale = 1.0f;
    bool freeze_rotation = false;
    bool freeze_x = false;
    bool freeze_y = false;
    core::Vec2 initial_velocity{};
    bool interpolate = true;   // Transform suave entre pasos
    bool allow_sleep = true;

    void reflect(ecs::PropertyVisitor& v);
};

struct Collider2DMaterial {
    bool is_trigger = false;
    float friction = 0.4f;
    float bounciness = 0.0f;
    bool one_way = false;  // plataforma de un sentido: solo choca desde su +Y
};

struct BoxCollider2D {
    Collider2DMaterial material{};
    core::Vec2 size{1.0f, 1.0f};
    core::Vec2 offset{};
    bool used_by_composite = false;

    void reflect(ecs::PropertyVisitor& v);
};

struct CircleCollider2D {
    Collider2DMaterial material{};
    float radius = 0.5f;
    core::Vec2 offset{};

    void reflect(ecs::PropertyVisitor& v);
};

// Poligono convexo (si no lo es, se usa su envolvente convexa). Hasta 16 puntos.
struct PolygonCollider2D {
    Collider2DMaterial material{};
    std::vector<core::Vec2> points{{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.0f, 0.5f}};
    core::Vec2 offset{};

    void reflect(ecs::PropertyVisitor& v);
};

struct CompositeCollider2D {
    Collider2DMaterial material{};

    void reflect(ecs::PropertyVisitor& v);
};

enum class ForceMode2D : int { Force = 0, Impulse = 1, VelocityChange = 2 };

enum class Physics2DEventType : int {
    CollisionEnter = 0,
    CollisionStay = 1,
    CollisionExit = 2,
    TriggerEnter = 3,
    TriggerStay = 4,
    TriggerExit = 5,
};

struct Physics2DEvent {
    Physics2DEventType type = Physics2DEventType::CollisionEnter;
    ecs::Entity a;
    ecs::Entity b;
    core::Vec3 point{};
    core::Vec3 normal{};             // de a hacia b
    core::Vec3 relative_velocity{};  // b respecto a a
    Physics2DEvent flipped() const {
        Physics2DEvent e = *this;
        std::swap(e.a, e.b);
        e.normal = -normal;
        e.relative_velocity = -relative_velocity;
        return e;
    }
};

struct RaycastHit2D {
    ecs::Entity entity;
    core::Vec2 point{};
    core::Vec2 normal{};
    float distance = 0.0f;
    float fraction = 0.0f;
};

// La simulacion de un World (la lleva System2D). Lo de dentro esta en
// Physics2D.cpp; aqui solo la interfaz.
class Physics2DWorld {
public:
    Physics2DWorld();
    ~Physics2DWorld();
    Physics2DWorld(const Physics2DWorld&) = delete;
    Physics2DWorld& operator=(const Physics2DWorld&) = delete;

    core::Vec2 gravity{0.0f, -9.81f};
    float fixed_step = 1.0f / 60.0f;
    int max_substeps = 5;
    int velocity_iterations = 8;
    int position_iterations = 3;

    // Los tilesets de los TilemapCollider2D (los de System2D).
    void setTilesetLibrary(TilesetLibrary* tilesets);

    // Crea los cuerpos de las entidades con colliders 2D.
    void start(ecs::World& world);
    void stop();
    bool running() const;
    // Avanza `dt` en pasos fijos (y recoge cuerpos nuevos o borrados).
    // Devuelve los pasos dados.
    int update(ecs::World& world, float dt);
    // Un paso (Pausa > Paso).
    void step(ecs::World& world);

    // Eventos desde la ultima llamada.
    std::vector<Physics2DEvent> takeEvents();

    // --- Cuerpos ---
    bool hasBody(ecs::Entity e) const;
    core::Vec2 velocity(ecs::Entity e) const;
    void setVelocity(ecs::Entity e, core::Vec2 v);
    float angularVelocity(ecs::Entity e) const;  // rad/s
    void setAngularVelocity(ecs::Entity e, float w);
    void addForce(ecs::Entity e, core::Vec2 force, ForceMode2D mode = ForceMode2D::Force);
    void addForceAtPosition(ecs::Entity e, core::Vec2 force, core::Vec2 point, ForceMode2D mode = ForceMode2D::Force);
    void addTorque(ecs::Entity e, float torque, ForceMode2D mode = ForceMode2D::Force);
    // Mueve el cuerpo ya (teletransporte), sin pasar por los choques.
    void setPosition(ecs::Entity e, core::Vec2 position);
    bool isSleeping(ecs::Entity e) const;
    void wakeUp(ecs::Entity e);
    // Rehace las formas de la entidad (cambio de collider o de tiles).
    void refresh(ecs::Entity e);

    // --- Consultas (mascara de capas como las de 3D) ---
    bool raycast(core::Vec2 origin, core::Vec2 direction, float distance, RaycastHit2D& hit,
                 std::uint32_t mask = 0xFFFFFFFFu, bool hit_triggers = false) const;
    std::vector<RaycastHit2D> raycastAll(core::Vec2 origin, core::Vec2 direction, float distance,
                                         std::uint32_t mask = 0xFFFFFFFFu, bool hit_triggers = false) const;
    std::vector<ecs::Entity> overlapCircle(core::Vec2 center, float radius, std::uint32_t mask = 0xFFFFFFFFu,
                                           bool hit_triggers = true) const;
    std::vector<ecs::Entity> overlapBox(core::Vec2 center, core::Vec2 size, float angle_degrees,
                                        std::uint32_t mask = 0xFFFFFFFFu, bool hit_triggers = true) const;
    std::vector<ecs::Entity> overlapPoint(core::Vec2 point, std::uint32_t mask = 0xFFFFFFFFu,
                                          bool hit_triggers = true) const;

    struct Stats {
        int bodies = 0;
        int shapes = 0;
        int contacts = 0;
        std::uint64_t steps = 0;
    };
    Stats stats() const;

    // Para el editor: formas en el mundo (contornos) de todo lo simulado.
    struct DebugShape {
        std::vector<core::Vec2> points;  // poligono (circulo: aproximado)
        bool trigger = false;
        bool sleeping = false;
        bool dynamic = false;
    };
    std::vector<DebugShape> debugShapes() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Formas de colision de una entidad en su espacio local 2D (para el gizmo
// del editor fuera de Play): poligonos y circulos.
struct ColliderOutline {
    std::vector<core::Vec2> points;  // en el mundo (plano XY)
    bool trigger = false;
};
std::vector<ColliderOutline> colliderOutlines(ecs::Entity e);

}  // namespace cramion::twod

#endif  // CRAMION_CORE_TWOD_PHYSICS2D_H
