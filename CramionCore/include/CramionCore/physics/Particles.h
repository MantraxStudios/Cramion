#ifndef CRAMION_CORE_PHYSICS_PARTICLES_H
#define CRAMION_CORE_PHYSICS_PARTICLES_H

// Sistema de particulas (como el ParticleSystem de Unity, con sus modulos
// principales): emision continua y rafagas, forma del emisor, velocidad,
// tamano y color a lo largo de la vida, gravedad, rozamiento y el modulo de
// Colision: las particulas chocan con los colliders de Jolt (rayos contra el
// mundo fisico filtrados por una mascara de capas), rebotan, pierden
// velocidad y vida, y envian eventos ParticleCollision (PhysicsSystem).
//
//   ParticleSystem   el componente (datos, se guarda en la escena)
//   ParticleWorld    la simulacion de todos los emisores del mundo (CPU) y
//                    lo que se le da al renderizador (gfx::ParticleDrawList)

#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsSettings.h"

#include <CramionFX/core/Math.h>
#include <CramionFX/vk/ParticleGeometry.h>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace cramion::physics {

class PhysicsSystem;

enum class EmitterShape : int { Cone = 0, Sphere = 1, Box = 2, Point = 3 };
enum class ParticleBlend : int { Alpha = 0, Additive = 1 };

struct ParticleSystem {
    // --- Principal ---
    bool play_on_start = true;  // empieza a emitir al entrar en Play
    bool looping = true;
    float duration = 5.0f;      // segundos de un ciclo de emision
    int max_particles = 1000;
    bool world_space = true;    // las vivas no siguen al emisor si se mueve
    bool preview_in_editor = true;  // simular en la vista de escena si esta seleccionado

    // --- Emision ---
    float rate = 40.0f;         // particulas por segundo
    int burst = 0;              // de golpe al empezar cada ciclo

    // --- Forma (en los ejes del emisor; sale hacia +Y) ---
    EmitterShape shape = EmitterShape::Cone;
    float cone_angle = 25.0f;   // grados
    float shape_radius = 0.1f;
    core::Vec3 box_size{1.0f, 1.0f, 1.0f};

    // --- Vida y movimiento ---
    float lifetime_min = 2.0f;
    float lifetime_max = 3.0f;
    float speed_min = 4.0f;
    float speed_max = 6.0f;
    float gravity_modifier = 1.0f;
    float drag = 0.0f;          // 1/s

    // --- Aspecto ---
    float size_start = 0.15f;   // diametro en metros
    float size_end = 0.05f;
    core::Vec3 color_start{1.0f, 0.75f, 0.35f};
    float alpha_start = 1.0f;
    core::Vec3 color_end{1.0f, 0.25f, 0.05f};
    float alpha_end = 0.0f;
    float intensity = 2.0f;     // multiplica el color (HDR: brilla con el bloom)
    ParticleBlend blend = ParticleBlend::Additive;

    // --- Colision ---
    bool collision = true;
    std::uint32_t collides_with = kAllLayers;
    float bounce = 0.4f;         // 0..1 velocidad normal que conserva
    float dampen = 0.1f;         // 0..1 velocidad que pierde en cada choque
    float lifetime_loss = 0.0f;  // 0..1 fraccion de vida que pierde (1 = muere)
    float radius_scale = 1.0f;   // radio de colision / radio visible
    bool send_collision_events = true;

    void reflect(ecs::PropertyVisitor& v);
};

// Simulacion de las particulas de un World. La lleva el editor (o el juego):
//
//   particles.update(world, dt, &physics);   // emite, mueve, choca
//   renderer.setParticles(particles.drawList(camera_position));
class ParticleWorld {
public:
    // Avanza todos los emisores activos. `physics` (puede ser nullptr) da
    // las colisiones y recibe los eventos ParticleCollision. `only` limita la
    // simulacion a esas entidades (vista previa en el editor); vacio = todas.
    void update(ecs::World& world, float delta_seconds, PhysicsSystem* physics,
                const std::vector<entt::entity>& only = {});
    // Borra todas las particulas y el estado de los emisores (al entrar o
    // salir de Play).
    void clear();

    // Origen flotante (ecs/FloatingOrigin.h): el mundo se desplazo -offset;
    // lo que guarda en coordenadas del mundo se mueve igual.
    void shiftOrigin(const core::Vec3& offset);

    // Reinicia / para / reanuda un emisor.
    void play(ecs::Entity entity);
    void stop(ecs::Entity entity, bool clear_particles = false);
    bool isPlaying(ecs::Entity entity) const;

    // Lo que dibuja el renderizador (las transparentes ordenadas de atras
    // adelante respecto a la camara).
    gfx::ParticleDrawList drawList(const core::Vec3& camera_position) const;

    std::size_t particleCount() const;
    std::size_t particleCount(ecs::Entity entity) const;
    std::uint64_t collisionCount() const { return collisions_; }

private:
    struct Particle {
        core::Vec3 position{};
        core::Vec3 velocity{};
        float age = 0.0f;
        float lifetime = 1.0f;
    };
    struct Emitter {
        std::vector<Particle> particles;
        float time = 0.0f;      // dentro del ciclo
        float pending = 0.0f;   // fraccion de particula por emitir
        bool playing = true;
        bool burst_done = false;
        bool started = false;
        core::Mat4 matrix = core::Mat4::identity();
        ParticleBlend blend = ParticleBlend::Additive;
        // Aspecto (copiado del componente para dibujar sin el World).
        float size_start = 0.1f, size_end = 0.1f;
        core::Vec4 color_start{}, color_end{};
        bool world_space = true;
    };

    void emit(Emitter& emitter, const ParticleSystem& settings, int count);

    std::unordered_map<entt::entity, Emitter> emitters_;
    std::mt19937 random_{0xC4A1105u};
    std::uint64_t collisions_ = 0;
};

}  // namespace cramion::physics

#endif  // CRAMION_CORE_PHYSICS_PARTICLES_H
