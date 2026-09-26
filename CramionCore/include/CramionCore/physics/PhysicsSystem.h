#ifndef CRAMION_CORE_PHYSICS_SYSTEM_H
#define CRAMION_CORE_PHYSICS_SYSTEM_H

// Simulacion fisica de un ecs::World con Jolt Physics.
//
//   PhysicsSystem physics;
//   physics.setSettings(settings);         // gravedad, capas... (PhysicsSettings.h)
//   physics.start(world);                  // crea los cuerpos desde los componentes
//   cada frame: physics.update(world, dt); // paso fijo por dentro (acumulador)
//   physics.stop();                        // destruye los cuerpos
//
// update(world, dt, /*simulate=*/false) solo lleva el mundo fisico al estado
// del World sin simular (modo edicion del editor: los raycast y la vista
// previa de particulas funcionan sin darle a Play).
//
// Dinamicos: la fisica escribe su posicion y rotacion en el Transform (en el
// mundo; respeta los padres). Cinematicos: se mueven hacia su Transform
// empujando a los dinamicos. Estaticos: fijos (si se mueve su Transform, se
// recolocan). Las entidades inactivas no tienen cuerpo.
//
// Entre pasos se detectan los cambios del World: entidades nuevas o borradas,
// componentes de fisica anadidos, quitados o editados, cambio de capa o de
// escala (se rehace el cuerpo).
//
// EVENTOS (como los mensajes OnCollision* / OnTrigger* / OnParticleCollision
// de Unity), en el hilo que llama a update(), tras cada paso fijo:
//
//   CollisionEnter / Stay / Exit   dos colliders solidos se tocan, siguen
//                                  tocandose (cada paso, si alguno esta
//                                  despierto) o se separan
//   TriggerEnter / Stay / Exit     un collider entra, sigue dentro o sale de
//                                  un trigger (`a` es el trigger)
//   ParticleCollision              una particula choca (`a` es el emisor)
//
// Se leen con events() (los del ultimo update) o con oyentes: addListener()
// recibe todos; addEntityListener() solo los de una entidad, con `a` = ella
// (como el script de ese objeto).
//
// Jolt no aparece en esta cabecera (pImpl): quien la incluye no necesita sus
// cabeceras.

#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSettings.h"

#include <CramionFX/asset/Model.h>
#include <CramionFX/core/Math.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace cramion::assets {
class AssetManager;
}
namespace cramion::terrain {
class TerrainData;
}

namespace cramion::physics {

// --- Eventos ------------------------------------------------------------------

enum class PhysicsEventType : int {
    CollisionEnter = 0,
    CollisionStay,
    CollisionExit,
    TriggerEnter,
    TriggerStay,
    TriggerExit,
    ParticleCollision,
};
inline constexpr int kPhysicsEventTypeCount = 7;
const char* eventTypeName(PhysicsEventType type);
bool isTriggerEvent(PhysicsEventType type);

struct PhysicsEvent {
    PhysicsEventType type = PhysicsEventType::CollisionEnter;
    ecs::Entity a;  // Collision: uno de los dos; Trigger: el trigger; Particle: el emisor
    ecs::Entity b;  // el otro
    // Contacto (Enter y Stay de colisiones y particulas; en Exit, el ultimo
    // conocido). La normal apunta de `a` hacia `b`.
    core::Vec3 point{};
    core::Vec3 normal{};
    core::Vec3 relative_velocity{};  // velocidad de b respecto a a en el punto
    float penetration = 0.0f;        // metros
    int contact_count = 0;           // puntos del contacto
    std::uint64_t step = 0;          // numero de paso fisico

    // El mismo evento visto desde `b` (a y b cambiados, la normal y la
    // velocidad relativa invertidas).
    PhysicsEvent flipped() const;
};

// --- Consultas ----------------------------------------------------------------

enum class QueryTriggers : int {
    UseGlobal = 0,  // PhysicsSettings::queries_hit_triggers
    Collide,        // tambien tocan triggers
    Ignore,         // solo colliders solidos
};

struct QueryFilter {
    std::uint32_t layer_mask = kDefaultRaycastLayers;
    QueryTriggers triggers = QueryTriggers::UseGlobal;
    ecs::Entity ignore;  // no tocar esta entidad (p. ej. quien dispara)
    bool record = true;  // se guarda para los gizmos (setRecordQueries)
};

struct RaycastHit {
    ecs::Entity entity;
    core::Vec3 point{};
    core::Vec3 normal{};
    float distance = 0.0f;
    bool trigger = false;
    int layer = 0;
};

// Consulta registrada para dibujarla (gizmos del editor).
struct QueryDebug {
    enum class Kind { Ray, SphereCast, OverlapSphere, OverlapBox } kind = Kind::Ray;
    core::Vec3 origin{};
    core::Vec3 end{};           // donde acaba el rayo (o el punto tocado)
    core::Vec3 extents{};       // radio (x) o semiejes de la caja
    core::Quat rotation{};
    bool hit = false;
    core::Vec3 hit_point{};
    core::Vec3 hit_normal{};
    int hits = 0;               // overlaps: cuantos
};

// --- Rigidbody -----------------------------------------------------------------

enum class ForceMode : int {
    Force = 0,       // N, continua (depende de la masa y del paso)
    Acceleration,    // m/s2, continua (no depende de la masa)
    Impulse,         // N*s, de golpe (depende de la masa)
    VelocityChange,  // m/s, de golpe (no depende de la masa)
};

// Punto de contacto para los gizmos (del ultimo paso).
struct ContactDebug {
    core::Vec3 point{};
    core::Vec3 normal{};
    bool trigger = false;
};

struct PhysicsStats {
    std::uint32_t bodies = 0;
    std::uint32_t active_bodies = 0;
    std::uint32_t contact_pairs = 0;
    std::uint32_t trigger_pairs = 0;
    std::uint64_t steps = 0;
    float step_milliseconds = 0.0f;  // del ultimo update (todos sus pasos)
};

class PhysicsSystem {
public:
    PhysicsSystem();
    ~PhysicsSystem();
    PhysicsSystem(const PhysicsSystem&) = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;

    // Se aplican al momento (gravedad, capas) o al siguiente start()
    // (max_bodies, hilos).
    void setSettings(const PhysicsSettings& settings);
    const PhysicsSettings& settings() const;

    // Geometria de los MeshCollider: la de la pieza del MeshRenderer de la
    // entidad, en el espacio local de la entidad. Con el renderizador en
    // marcha, las piezas ya se movieron a la escena de CramionFX: el editor
    // conecta aqui RenderSync::actorModelData(entity, scene). Sin proveedor
    // (o si devuelve nullptr) se pide al AssetManager.
    using MeshProvider = std::function<const asset::ModelData*(ecs::Entity)>;
    void setMeshProvider(MeshProvider provider);
    void setAssetManager(assets::AssetManager* manager);
    // Datos de los terrenos (componente Terrain con colision): su collider es
    // un campo de alturas. Se rehace cuando sube TerrainData::collisionVersion.
    using TerrainProvider = std::function<std::shared_ptr<const terrain::TerrainData>(ecs::Entity)>;
    void setTerrainProvider(TerrainProvider provider);

    // Geometria estatica sin entidad (los mundos de bloques): una malla por
    // clave, `triangles` = lista de triangulos (3 puntos, antihorarios vistos
    // desde fuera) relativos a `origin`. Sobrevive a stop()/start(): se crea
    // de nuevo en cada mundo fisico. Choca como el suelo (sin eventos) y los
    // rayos la tocan (hit.entity vacia).
    void setStaticMesh(std::uint64_t key, const core::Vec3& origin, std::vector<core::Vec3> triangles, int layer = 0);
    void removeStaticMesh(std::uint64_t key);
    void clearStaticMeshes();
    std::size_t staticMeshCount() const;

    // Crea el mundo fisico y los cuerpos. stop() lo destruye todo.
    void start(ecs::World& world);
    void stop();
    bool running() const;
    // Origen flotante (ecs/FloatingOrigin.h): el mundo se desplazo -offset;
    // todos los cuerpos de Jolt (con su velocidad) y lo que guarda en coordenadas del mundo se mueve igual.
    void shiftOrigin(const core::Vec3& offset);


    // Lleva el mundo fisico al World y, si `simulate`, avanza `delta_seconds`
    // en pasos fijos (acumulador) y vuelca los resultados en los Transform.
    // Devuelve los pasos dados.
    int update(ecs::World& world, float delta_seconds, bool simulate = true);
    // Un unico paso fijo (el boton "Paso" del modo Play en pausa).
    void singleStep(ecs::World& world);

    // --- Consultas (filtradas por capas; ver QueryFilter) ---
    bool raycast(const core::Vec3& origin, const core::Vec3& direction, float max_distance,
                 RaycastHit& hit, const QueryFilter& filter = {}) const;
    // Todos los que toca, del mas cercano al mas lejano.
    std::vector<RaycastHit> raycastAll(const core::Vec3& origin, const core::Vec3& direction,
                                       float max_distance, const QueryFilter& filter = {}) const;
    // Una esfera que avanza por el rayo: el primero que toca.
    bool sphereCast(const core::Vec3& origin, float radius, const core::Vec3& direction,
                    float max_distance, RaycastHit& hit, const QueryFilter& filter = {}) const;
    // Entidades cuyos cuerpos solapan una esfera / una caja.
    std::vector<ecs::Entity> overlapSphere(const core::Vec3& center, float radius,
                                           const QueryFilter& filter = {}) const;
    std::vector<ecs::Entity> overlapBox(const core::Vec3& center, const core::Vec3& half_extents,
                                        const core::Quat& rotation,
                                        const QueryFilter& filter = {}) const;

    // Guarda las ultimas consultas para dibujarlas (gizmos). Las que llevan
    // QueryFilter::record = false (las de las particulas) no se guardan.
    void setRecordQueries(bool record);
    const std::vector<QueryDebug>& recordedQueries() const;
    void clearRecordedQueries();

    // --- Rigidbody (dinamicos; en los demas no hacen nada) ---
    core::Vec3 linearVelocity(ecs::Entity entity) const;

    // --- Vehiculos (componentes Vehicle + WheelCollider) ---
    // Acelerador -1..1 (negativo = marcha atras), direccion -1..1 (derecha +),
    // freno y freno de mano 0..1. Hasta que se vuelva a llamar.
    void setVehicleInput(ecs::Entity vehicle, float throttle, float steering, float brake, float handbrake);
    // Los que tienen "Conducir con teclado" (W/S, A/D, Espacio).
    void driveVehiclesWithKeyboard(ecs::World& world, bool forward, bool back, bool left, bool right, bool handbrake);
    struct VehicleState {
        bool valid = false;
        float speed_kmh = 0.0f;
        float rpm = 0.0f;
        int gear = 0;
        int wheels_on_ground = 0;
    };
    VehicleState vehicleState(ecs::Entity vehicle) const;
    void setLinearVelocity(ecs::Entity entity, const core::Vec3& velocity);
    core::Vec3 angularVelocity(ecs::Entity entity) const;
    void setAngularVelocity(ecs::Entity entity, const core::Vec3& velocity);
    void addForce(ecs::Entity entity, const core::Vec3& force, ForceMode mode = ForceMode::Force);
    void addForceAtPosition(ecs::Entity entity, const core::Vec3& force, const core::Vec3& position,
                            ForceMode mode = ForceMode::Force);
    void addTorque(ecs::Entity entity, const core::Vec3& torque, ForceMode mode = ForceMode::Force);
    // Compatibilidad: addForce(..., ForceMode::Impulse).
    void addImpulse(ecs::Entity entity, const core::Vec3& impulse);
    bool isSleeping(ecs::Entity entity) const;
    void wakeUp(ecs::Entity entity);
    void sleep(ecs::Entity entity);
    bool hasBody(ecs::Entity entity) const;
    // Centro de masas en el mundo.
    core::Vec3 centerOfMass(ecs::Entity entity) const;

    // --- Eventos ---
    using EventCallback = std::function<void(const PhysicsEvent&)>;
    // Los del ultimo update() (se vacia al empezar cada update).
    const std::vector<PhysicsEvent>& events() const;
    int addListener(EventCallback callback);
    int addEntityListener(ecs::Entity entity, EventCallback callback);
    void removeListener(int id);
    // Para otros sistemas (las particulas): lo anade a events() y avisa a los
    // oyentes.
    void reportEvent(const PhysicsEvent& event);

    // --- Depuracion ---
    // Contactos del ultimo paso (para dibujarlos).
    const std::vector<ContactDebug>& contactPoints() const;
    // Triangulos (en el mundo) de la forma que usa la fisica para la entidad
    // (solido y trigger). false si no tiene cuerpo. Tope de triangulos.
    bool bodyTriangles(ecs::Entity entity, std::vector<core::Vec3>& triangles,
                       std::size_t max_triangles = 20000) const;
    PhysicsStats stats() const;
    std::uint32_t bodyCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::physics

#endif  // CRAMION_CORE_PHYSICS_SYSTEM_H
