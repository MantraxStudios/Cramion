#ifndef CRAMION_CORE_NAVIGATION_H
#define CRAMION_CORE_NAVIGATION_H

// Navegacion (navmesh) como la de Unreal Engine, con Recast/Detour (la misma
// biblioteca que usa Unreal):
//
//   NavMeshBounds   el volumen (NavMeshBoundsVolume): dentro se genera la
//                   malla de navegacion. Sin ninguno no hay navmesh. Caja
//                   alineada con los ejes del mundo (posicion y escala de la
//                   entidad por `size`; el giro no cuenta).
//   NavModifier     caja que quita la navegacion de su interior (Bloquear,
//                   como NavArea_Null) o la encarece (Evitar: los caminos la
//                   rodean si pueden, como NavArea_Obstacle). Gira en Y.
//   NavAgent        lo que se mueve por la malla (el CharacterMovement +
//                   AIController de Unreal): moveTo(destino), velocidad,
//                   aceleracion y evitar a los demas agentes (DetourCrowd).
//
// La malla sale de la GEOMETRIA DE COLISION (como en Unreal, no de lo que se
// dibuja): los colliders de caja, esfera, capsula, malla y plano y los
// terrenos con colision. No cuentan los triggers, los Rigidbody dinamicos ni
// los agentes (y sus hijos).
//
// Generacion en tiempo real ("Runtime Generation: Dynamic" de Unreal): la
// malla esta dividida en baldosas (tiles) y al mover, crear o borrar un
// collider solo se rehacen las baldosas que toca, en hilos de fondo, sin
// parar el editor ni el juego. Funciona igual en el editor, en Play y en el
// juego exportado.
//
// Ajustes del proyecto (el agente y la resolucion de la malla) en
// ProjectSettings/Navigation.json.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace cramion::asset {
struct ModelData;
}
namespace cramion::terrain {
class TerrainData;
}
namespace cramion::physics {
class PhysicsSystem;
}

namespace cramion::navigation {

// --- Componentes -------------------------------------------------------------

struct NavMeshBounds {
    core::Vec3 size{40.0f, 12.0f, 40.0f};  // metros (por la escala de la entidad)

    void reflect(ecs::PropertyVisitor& v);
};

enum class NavArea : int { Block = 0, Avoid = 1 };

struct NavModifier {
    core::Vec3 size{4.0f, 4.0f, 4.0f};  // metros (por la escala de la entidad)
    NavArea area = NavArea::Block;

    void reflect(ecs::PropertyVisitor& v);
};

struct NavAgent {
    float speed = 3.5f;           // m/s
    float acceleration = 12.0f;   // m/s^2
    float radius = 0.35f;         // para esquivar a otros agentes
    float height = 1.8f;
    float stopping_distance = 0.15f;  // llega cuando esta a esta distancia
    bool rotate_to_movement = true;   // mira hacia donde camina (en Y)
    float turn_speed = 540.0f;        // grados por segundo
    bool avoidance = true;            // esquiva a los demas agentes
    float base_offset = 0.0f;         // altura del pivote sobre la malla

    void reflect(ecs::PropertyVisitor& v);
};

// Registra NavMeshBounds, NavModifier y NavAgent (idempotente).
void registerNavigationComponents();

// --- Ajustes del proyecto -------------------------------------------------------

struct NavigationSettings {
    // El agente para el que se genera la malla (el "Supported Agent" de Unreal).
    float agent_radius = 0.35f;     // distancia a las paredes
    float agent_height = 1.8f;      // altura libre minima
    float max_step_height = 0.45f;  // escalones que sube
    float max_slope = 45.0f;        // grados de pendiente que sube

    // Resolucion.
    float cell_size = 0.15f;       // metros (horizontal): menos = mas detalle
    float cell_height = 0.10f;     // metros (vertical)
    int tile_size = 64;            // celdas por lado de cada baldosa
    float min_region_area = 2.0f;  // m^2: islas mas pequenas se quitan
    float detail_sample_distance = 6.0f;  // en celdas: ajuste de la malla al relieve
    float detail_max_error = 1.0f;        // en celdas de altura

    float avoid_cost = 10.0f;  // coste de las zonas "Evitar" (1 = como el suelo)
    int max_agents = 256;
    bool runtime_generation = true;  // rehacer al cambiar la escena (en Play y en el juego)
    int build_threads = 0;           // 0 = automatico

    // Lo que cambia la malla (no el coste ni los agentes).
    bool sameGeometry(const NavigationSettings& o) const;
    float tileWorldSize() const { return cell_size * static_cast<float>(tile_size); }
};

bool loadNavigationSettings(const std::filesystem::path& file, NavigationSettings& settings);
bool saveNavigationSettings(const std::filesystem::path& file, const NavigationSettings& settings);

// --- Sistema -------------------------------------------------------------------

struct NavPathPoint {
    core::Vec3 position{};
};

struct NavStats {
    int tiles = 0;          // baldosas con malla
    int polygons = 0;
    int pending_tiles = 0;  // en cola o generandose
    int agents = 0;
    float last_tile_ms = 0.0f;  // ultima baldosa generada
    std::size_t memory_bytes = 0;
};

// Malla para verla en el editor (verde, como la tecla P de Unreal): los
// triangulos de la malla de detalle (un poco por encima del suelo) y el
// borde de la zona navegable. `version` sube con cada cambio.
struct NavDebugMesh {
    std::vector<core::Vec3> triangles;  // de 3 en 3
    std::vector<std::uint8_t> triangle_area;  // una por triangulo (0 suelo, 1 evitar)
    std::vector<core::Vec3> edges;      // de 2 en 2
    std::uint64_t version = 0;
};

class NavigationSystem {
public:
    NavigationSystem();
    ~NavigationSystem();
    NavigationSystem(const NavigationSystem&) = delete;
    NavigationSystem& operator=(const NavigationSystem&) = delete;

    // Cambiar la resolucion o el agente rehace toda la malla.
    void setSettings(const NavigationSettings& settings);
    const NavigationSettings& settings() const;

    // Geometria de los MeshCollider y terrenos: los mismos proveedores que la
    // fisica (PhysicsSystem::setMeshProvider / setTerrainProvider).
    using MeshProvider = std::function<const asset::ModelData*(ecs::Entity)>;
    using TerrainProvider = std::function<std::shared_ptr<const terrain::TerrainData>(ecs::Entity)>;
    void setMeshProvider(MeshProvider provider);
    void setTerrainProvider(TerrainProvider provider);
    // Agentes con Rigidbody dinamico: se mueven por velocidad (si no, por el Transform).
    void setPhysics(physics::PhysicsSystem* physics);

    // Cada frame: busca cambios en la escena, lanza las baldosas afectadas en
    // los hilos de fondo y mete las terminadas. `simulate_agents`: mueve los
    // agentes (Play / juego). Con `rebuild` = false (Runtime Generation
    // desactivada) la malla no cambia.
    void update(ecs::World& world, float delta_seconds, bool simulate_agents, bool rebuild = true);

    // Espera a que no quede ninguna baldosa pendiente (pruebas, cargar un
    // nivel). Hace update() sin agentes mientras espera.
    void waitForBuild(ecs::World& world, float timeout_seconds = 30.0f);
    // Tira la malla y la vuelve a generar entera.
    void rebuildAll();
    // Sin malla, sin agentes ni cache (al cambiar de escena).
    void clear();
    // Quita los agentes (al salir de Play); la malla se queda.
    void resetAgents();

    bool ready() const;  // hay alguna baldosa
    bool building() const;
    NavStats stats() const;
    const NavDebugMesh& debugMesh() const;

    // --- Consultas (mundo) ---
    // Camino por la malla (puntos de giro, del inicio al final). false si no
    // hay. `partial`: si el destino no se alcanza, hasta lo mas cerca.
    // Origen flotante (ecs/FloatingOrigin.h): el mundo se desplazo -offset.
    // La malla no se rehace: vive en su propio espacio y las consultas
    // convierten al entrar y al salir.
    void shiftOrigin(const core::Vec3& offset);
    // Lo que hay que sumar a debugMesh() para llevarlo al espacio del mundo.
    core::Vec3 navToLocal() const;

    bool findPath(const core::Vec3& from, const core::Vec3& to, std::vector<core::Vec3>& path,
                  bool* partial = nullptr) const;
    // Punto de la malla mas cercano (buscando `extent` m alrededor).
    bool projectPoint(const core::Vec3& point, core::Vec3& result, float extent = 2.0f) const;
    // Punto al azar de la malla a menos de `radius` de `center`.
    bool randomPoint(const core::Vec3& center, float radius, core::Vec3& result) const;
    // Linea recta por la malla: false si choca con un borde (`hit` = donde).
    bool raycast(const core::Vec3& from, const core::Vec3& to, core::Vec3* hit = nullptr) const;

    // --- Agentes (NavAgent) ---
    // Se guarda aunque la malla aun no este lista: arranca en cuanto lo este.
    bool moveTo(ecs::Entity agent, const core::Vec3& target);
    void stop(ecs::Entity agent);
    bool isMoving(ecs::Entity agent) const;
    float remainingDistance(ecs::Entity agent) const;  // por el camino
    core::Vec3 agentVelocity(ecs::Entity agent) const;
    // Esquinas del camino actual del agente (para verlo en el editor).
    std::vector<core::Vec3> agentPath(ecs::Entity agent) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::navigation

#endif  // CRAMION_CORE_NAVIGATION_H
