#ifndef CRAMION_CORE_FLUID_H
#define CRAMION_CORE_FLUID_H

// Liquidos por particulas (como Obi Fluid / FleX / Niagara Fluids): agua,
// aceite, miel, lava, barro, sangre, acido... La simulacion (Position Based
// Fluids) y la superficie corren en la GPU (CramionFX/vk/FluidPass.h); aqui
// estan los componentes y el sistema que lleva el mundo al renderizador:
//
//   FluidWorld    ajustes del mundo de liquidos (dominio, radio de las
//                 particulas, gravedad, iteraciones, choques, aspecto y el
//                 liquido "Personalizado"). Uno por escena (el primero).
//   FluidEmitter  de donde sale el liquido: boquilla (chorro), caja o esfera
//                 (se llenan de golpe). Tipo, caudal, velocidad, vida...
//   FluidDrain    caja donde el liquido desaparece (desague).
//   FluidSystem   cada frame: formas de choque (Box/Sphere/Capsule/Plane/
//                 Mesh Collider como caja y el terreno como campo de
//                 alturas), particulas nuevas, subpasos fijos; con lo que
//                 vuelve de la GPU, flotacion y arrastre de los Rigidbody.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>
#include <CramionFX/vk/FluidPass.h>

#include <array>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::gfx {
class VulkanRenderer;
}
namespace cramion::physics {
class PhysicsSystem;
}
namespace cramion::terrain {
class TerrainStore;
}

namespace cramion::fluid {

enum class FluidType : int { Water = 0, Oil, Honey, Lava, Mud, Blood, Acid, Custom, Count };
enum class EmitterShape : int { Nozzle = 0, Box = 1, Sphere = 2 };

// Nombres visibles (Agua, Aceite, Miel...) y claves para Lua/MCP (water, oil...).
const std::array<const char*, static_cast<std::size_t>(FluidType::Count)>& fluidTypeNames();
const std::array<const char*, static_cast<std::size_t>(FluidType::Count)>& fluidTypeKeys();
// Por nombre o clave (sin acentos ni mayusculas); -1 si no existe.
int fluidTypeFromName(const std::string& name);
// Densidad real (kg/m3) de cada tipo: la flotacion.
float fluidDensity(FluidType type);
// Material de fabrica de un tipo (color, absorcion, viscosidad...).
gfx::FluidMaterial fluidPreset(FluidType type);

struct FluidWorld {
    core::Vec3 size{8.0f, 6.0f, 8.0f};  // dominio (m), centrado en la entidad
    bool solid_walls = false;           // el dominio es un tanque invisible
    bool kill_outside = true;           // lo que sale del dominio desaparece
    float particle_radius = 0.04f;      // m (separacion = 2 * radio)
    int max_particles = 65536;
    core::Vec3 gravity{0.0f, -9.81f, 0.0f};
    int iterations = 3;
    float substep_rate = 120.0f;  // Hz (pasos fijos)
    int max_substeps = 3;         // por frame (si el frame tarda mas, se frena)
    float friction = 0.1f;
    float surface_tension = 1.0f;
    float max_speed = 25.0f;

    // Choques e interaccion.
    bool collide_scene = true;    // Box/Sphere/Capsule/Plane/Mesh Collider
    bool collide_terrain = true;  // terrenos (campo de alturas)
    bool push_bodies = true;      // flotacion y arrastre de los Rigidbody
    float buoyancy = 1.0f;
    float drag = 1.0f;
    bool simulate_in_editor = false;  // "Simular en el editor" (fuera de Play)

    // Aspecto.
    float render_radius = 1.3f;
    float smoothing = 1.0f;
    float thickness = 1.0f;
    float refraction = 1.0f;
    bool debug_particles = false;

    // Liquido "Personalizado".
    core::Vec3 custom_color{0.30f, 0.05f, 0.45f};       // color difuso
    core::Vec3 custom_absorption{2.0f, 6.0f, 1.5f};     // 1/m
    core::Vec3 custom_emission{0.0f, 0.0f, 0.0f};       // HDR
    float custom_viscosity = 0.2f;
    float custom_cohesion = 1.0f;
    float custom_density = 1000.0f;  // kg/m3

    void reflect(ecs::PropertyVisitor& v);
};

struct FluidEmitter {
    FluidType fluid = FluidType::Water;
    EmitterShape shape = EmitterShape::Nozzle;
    bool emitting = true;
    core::Vec3 direction{0.0f, -1.0f, 0.0f};  // local (gira con la entidad)
    float speed = 2.5f;            // m/s
    float nozzle_radius = 0.08f;   // boquilla (m)
    float rate = 0.0f;             // particulas/s (0 = chorro lleno)
    float spread = 2.0f;           // grados de dispersion
    core::Vec3 size{0.8f, 0.8f, 0.8f};  // caja (m)
    float radius = 0.4f;           // esfera (m)
    bool repeat = false;           // caja/esfera: volver a llenar cada `interval`
    float interval = 2.0f;         // s
    float delay = 0.0f;            // s antes de empezar
    float lifetime = 0.0f;         // s de vida de cada particula (0 = siempre)
    int max_total = 0;             // particulas en total (0 = sin limite)

    void reflect(ecs::PropertyVisitor& v);
};

struct FluidDrain {
    core::Vec3 size{1.0f, 0.5f, 1.0f};
    void reflect(ecs::PropertyVisitor& v);
};

void registerFluidComponents();

struct FluidSystemStats {
    std::uint32_t particles = 0;
    std::uint32_t capacity = 0;
    std::uint32_t substeps = 0;
    std::uint32_t shapes = 0;
    std::uint32_t emitters = 0;
    std::uint32_t floating_bodies = 0;
    std::uint64_t memory_bytes = 0;
    bool active = false;
    bool simulating = false;
};

class FluidSystem {
public:
    FluidSystem() = default;
    ~FluidSystem();
    FluidSystem(const FluidSystem&) = delete;
    FluidSystem& operator=(const FluidSystem&) = delete;

    // "Simular en el editor" del primer FluidWorld activo (fuera de Play).
    static bool previewInEditor(ecs::World& world);

    void setTerrainStore(terrain::TerrainStore* store) { terrain_store_ = store; }
    // Caja local de la malla de una entidad (para los Mesh Collider).
    using BoundsProvider = std::function<bool(ecs::Entity, core::Vec3& min, core::Vec3& max)>;
    void setBoundsProvider(BoundsProvider provider) { bounds_provider_ = std::move(provider); }

    // Cada frame. `simulate`: avanza el tiempo (Play o "Simular en el
    // editor"); si no, el liquido se queda quieto (pero se ve).
    void update(ecs::World& world, float delta_seconds, bool simulate, physics::PhysicsSystem* physics,
                gfx::VulkanRenderer& renderer);
    // Borra las particulas y el estado de los emisores (al entrar o salir de Play).
    void clear();
    void shiftOrigin(const core::Vec3& offset);

    // --- API (Lua, MCP) ---
    // `count` particulas en una esfera alrededor de `position` (se reparten
    // en una red); `radius` <= 0: el justo para que quepan.
    void spawn(const core::Vec3& position, int count, FluidType type, const core::Vec3& velocity = {},
               float radius = 0.0f, float lifetime = 0.0f);
    // Reinicia un emisor (vuelve a llenar una caja/esfera).
    void restart(ecs::Entity emitter);
    std::uint32_t particleCount() const { return stats_.particles; }
    std::uint32_t particleCount(FluidType type) const;
    // Densidad relativa en un punto (0 = sin liquido, ~1 = lleno) con las
    // particulas a menos de `radius` (por defecto 2 separaciones).
    float densityAt(const core::Vec3& position, float radius = 0.0f) const;
    core::Vec3 velocityAt(const core::Vec3& position, float radius = 0.0f) const;
    // Altura de la superficie del liquido sobre (x, z); false si no hay.
    bool surfaceHeight(float x, float z, float& height, float radius = 0.0f) const;
    const FluidSystemStats& stats() const { return stats_; }
    float particleRadius() const { return radius_; }

private:
    struct EmitterState {
        float time = 0.0f;
        float layer_time = 0.0f;
        float refill = 0.0f;
        int emitted = 0;
        float rate_carry = 0.0f;  // fraccion de particula del caudal
        bool filled = false;
        std::uint32_t layer = 0;
    };
    void emit(ecs::Entity entity, const FluidEmitter& emitter, EmitterState& state, float dt);
    void fillVolume(const core::Mat4& matrix, const FluidEmitter& emitter, EmitterState& state);
    void gatherShapes(ecs::World& world, const core::Vec3& lo, const core::Vec3& hi);
    void updateHeightfield(ecs::World& world, const core::Vec3& lo, const core::Vec3& hi, gfx::FluidPass& pass);
    void applyBodies(ecs::World& world, physics::PhysicsSystem& physics, const FluidWorld& settings, float dt);
    void indexSnapshot(const gfx::FluidSnapshot& snapshot);
    template <typename Fn>
    void forNear(const core::Vec3& p, float radius, Fn&& fn) const;
    template <typename Fn>
    void forBox(const core::Vec3& lo, const core::Vec3& hi, Fn&& fn) const;
    void queue(const gfx::FluidSpawn& s);

    terrain::TerrainStore* terrain_store_ = nullptr;
    BoundsProvider bounds_provider_;
    std::unordered_map<entt::entity, EmitterState> emitters_;
    std::vector<gfx::FluidSpawn> pending_;  // pendientes (llenados grandes van por partes)
    float accumulator_ = 0.0f;
    float radius_ = 0.04f;
    float substep_ = 1.0f / 120.0f;
    std::mt19937 random_{0xF1D1u};
    FluidSystemStats stats_{};
    bool clear_requested_ = false;
    core::Vec3 shift_{};
    std::vector<gfx::FluidShape> shapes_;
    struct BodyShape {
        entt::entity entity = entt::null;
        core::Vec3 min{}, max{};
        float volume = 0.0f;
        std::uint32_t first = 0;  // sus formas en shapes_
        std::uint32_t count = 0;
    };
    std::vector<BodyShape> bodies_;
    std::uint64_t height_key_ = 0;

    // Copia de las particulas (un par de frames tarde) con una rejilla para
    // las consultas.
    std::vector<core::Vec4> positions_;
    std::vector<core::Vec4> velocities_;
    std::uint64_t snapshot_frame_ = ~0ull;
    float cell_ = 0.2f;
    // Rejilla hash (se rehace al consultar si llego una lectura nueva).
    void buildGrid() const;
    std::uint64_t cellKey(int x, int y, int z) const;
    mutable std::vector<std::uint32_t> grid_start_;  // tabla + 1
    mutable std::vector<std::uint32_t> grid_items_;
    mutable bool grid_dirty_ = true;
    std::array<std::uint32_t, static_cast<std::size_t>(FluidType::Count)> type_counts_{};
    float custom_density_ = 1000.0f;
    // Sin FluidWorld: el dominio se centra en el primer emisor o en lo ultimo creado desde Lua.
    core::Vec3 fallback_center_{0.0f, 2.0f, 0.0f};
    bool has_fallback_ = false;
    bool was_active_ = false;
};

// El sistema activo (el del editor o el juego): lo usan Lua y las consultas.
FluidSystem* activeSystem();
void setActiveSystem(FluidSystem* system);

}  // namespace cramion::fluid

#endif  // CRAMION_CORE_FLUID_H
