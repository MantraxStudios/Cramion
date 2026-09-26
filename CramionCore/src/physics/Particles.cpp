#include "CramionCore/physics/Particles.h"

#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/physics/PhysicsSystem.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace cramion::physics {

using core::Vec3;
using core::Vec4;
using ecs::FloatRange;
using ecs::Vec3Kind;

void ParticleSystem::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 4> kShapes = {"Cono", "Esfera", "Caja", "Punto"};
    static constexpr std::array<const char*, 2> kBlends = {"Transparente", "Aditiva"};
    const bool all = v.wantsAllFields();
    if (v.beginGroup("Principal", true)) {
        v.field({"play_on_start", "Empezar al dar Play"}, play_on_start);
        v.field({"looping", "En bucle"}, looping);
        v.field({"duration", "Duracion", "Segundos de un ciclo de emision"}, duration,
                FloatRange{0.05f, 1000.0f, 0.05f, "%.2f s"});
        v.field({"max_particles", "Maximo de particulas"}, max_particles, 1, 100000);
        v.field({"world_space", "Espacio del mundo",
                 "Las particulas vivas se quedan donde nacieron aunque el emisor se mueva"},
                world_space);
        v.field({"preview_in_editor", "Vista previa en el editor",
                 "Simular en la vista de escena mientras esta seleccionado (sin Play)"},
                preview_in_editor);
        v.endGroup();
    }
    if (v.beginGroup("Emision", true)) {
        v.field({"rate", "Por segundo"}, rate, FloatRange{0.0f, 10000.0f, 0.5f, "%.1f"});
        v.field({"burst", "Rafaga al empezar"}, burst, 0, 100000);
        v.endGroup();
    }
    if (v.beginGroup("Forma", true)) {
        ecs::enumField(v, {"shape", "Forma", "Las particulas salen hacia +Y del emisor"}, shape, kShapes);
        if (all || shape == EmitterShape::Cone) {
            v.field({"cone_angle", "Angulo del cono"}, cone_angle, FloatRange{0.0f, 90.0f, 0.5f, "%.1f°", true});
        }
        if (all || shape == EmitterShape::Cone || shape == EmitterShape::Sphere) {
            v.field({"shape_radius", "Radio"}, shape_radius, FloatRange{0.0f, 1000.0f, 0.01f, "%.2f m"});
        }
        if (all || shape == EmitterShape::Box) {
            v.field({"box_size", "Tamano de la caja"}, box_size, Vec3Kind::Scale);
        }
        v.endGroup();
    }
    if (v.beginGroup("Vida y movimiento", true)) {
        v.field({"lifetime_min", "Vida minima"}, lifetime_min, FloatRange{0.01f, 1000.0f, 0.05f, "%.2f s"});
        v.field({"lifetime_max", "Vida maxima"}, lifetime_max, FloatRange{0.01f, 1000.0f, 0.05f, "%.2f s"});
        v.field({"speed_min", "Velocidad minima"}, speed_min, FloatRange{0.0f, 1000.0f, 0.05f, "%.2f m/s"});
        v.field({"speed_max", "Velocidad maxima"}, speed_max, FloatRange{0.0f, 1000.0f, 0.05f, "%.2f m/s"});
        v.field({"gravity_modifier", "Gravedad", "Multiplica la gravedad de la fisica"}, gravity_modifier,
                FloatRange{-10.0f, 10.0f, 0.01f, "%.2f"});
        v.field({"drag", "Rozamiento del aire"}, drag, FloatRange{0.0f, 50.0f, 0.01f, "%.2f"});
        v.endGroup();
    }
    if (v.beginGroup("Aspecto", true)) {
        v.field({"size_start", "Tamano al nacer"}, size_start, FloatRange{0.0f, 100.0f, 0.005f, "%.3f m"});
        v.field({"size_end", "Tamano al morir"}, size_end, FloatRange{0.0f, 100.0f, 0.005f, "%.3f m"});
        v.field({"color_start", "Color al nacer"}, color_start, Vec3Kind::Color);
        v.field({"alpha_start", "Opacidad al nacer"}, alpha_start, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"color_end", "Color al morir"}, color_end, Vec3Kind::Color);
        v.field({"alpha_end", "Opacidad al morir"}, alpha_end, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"intensity", "Intensidad", "Mas de 1: brillan (bloom)"}, intensity,
                FloatRange{0.0f, 100.0f, 0.05f, "%.2f"});
        ecs::enumField(v, {"blend", "Mezcla"}, blend, kBlends);
        v.endGroup();
    }
    if (v.beginGroup("Colision", true)) {
        v.field({"collision", "Chocar con el mundo", "Con los colliders de fisica (no con los triggers)"}, collision);
        if (all || collision) {
            v.layerMask({"collides_with", "Chocar con", "Capas contra las que chocan"}, collides_with);
            v.field({"bounce", "Rebote"}, bounce, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
            v.field({"dampen", "Perdida de velocidad"}, dampen, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
            v.field({"lifetime_loss", "Perdida de vida", "1 = mueren al chocar"}, lifetime_loss,
                    FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
            v.field({"radius_scale", "Escala del radio"}, radius_scale, FloatRange{0.0f, 10.0f, 0.01f, "%.2f"});
            v.field({"send_collision_events", "Enviar eventos",
                     "Eventos ParticleCollision (el emisor y lo que toca)"},
                    send_collision_events);
        }
        v.endGroup();
    }
}

// -----------------------------------------------------------------------------
// Simulacion
// -----------------------------------------------------------------------------

namespace {

Vec4 colorOf(const Vec3& rgb, float alpha, float intensity) {
    return Vec4{rgb.x * intensity, rgb.y * intensity, rgb.z * intensity, alpha};
}

Vec4 lerp4(const Vec4& a, const Vec4& b, float t) {
    return Vec4{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

}  // namespace

void ParticleWorld::emit(Emitter& emitter, const ParticleSystem& s, int count) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const int room = std::max(s.max_particles, 0) - static_cast<int>(emitter.particles.size());
    count = std::min(count, room);
    for (int i = 0; i < count; ++i) {
        Vec3 position{};
        Vec3 direction{0.0f, 1.0f, 0.0f};
        const float phi = unit(random_) * 2.0f * core::kPi;
        switch (s.shape) {
            case EmitterShape::Cone: {
                const float r = s.shape_radius * std::sqrt(unit(random_));
                position = Vec3{std::cos(phi) * r, 0.0f, std::sin(phi) * r};
                // Direccion uniforme en el casquete del cono.
                const float max_angle = core::radians(std::clamp(s.cone_angle, 0.0f, 90.0f));
                const float cos_theta = 1.0f - unit(random_) * (1.0f - std::cos(max_angle));
                const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
                const float spin = unit(random_) * 2.0f * core::kPi;
                direction = Vec3{std::cos(spin) * sin_theta, cos_theta, std::sin(spin) * sin_theta};
                break;
            }
            case EmitterShape::Sphere:
            case EmitterShape::Point: {
                const float z = unit(random_) * 2.0f - 1.0f;
                const float rxy = std::sqrt(std::max(0.0f, 1.0f - z * z));
                direction = Vec3{std::cos(phi) * rxy, z, std::sin(phi) * rxy};
                if (s.shape == EmitterShape::Sphere) {
                    position = direction * (s.shape_radius * std::cbrt(unit(random_)));
                }
                break;
            }
            case EmitterShape::Box:
                position = Vec3{(unit(random_) - 0.5f) * s.box_size.x, (unit(random_) - 0.5f) * s.box_size.y,
                                (unit(random_) - 0.5f) * s.box_size.z};
                break;
        }
        const float speed = s.speed_min + (s.speed_max - s.speed_min) * unit(random_);
        Particle p;
        if (emitter.world_space) {
            p.position = ecs::transformPoint(emitter.matrix, position);
            p.velocity = core::normalize(ecs::transformDirection(emitter.matrix, direction)) * speed;
        } else {
            p.position = position;
            p.velocity = direction * speed;
        }
        p.lifetime = std::max(s.lifetime_min + (s.lifetime_max - s.lifetime_min) * unit(random_), 0.01f);
        emitter.particles.push_back(p);
    }
}

void ParticleWorld::update(ecs::World& world, float delta_seconds, PhysicsSystem* physics,
                           const std::vector<entt::entity>& only) {
    const float dt = std::clamp(delta_seconds, 0.0f, 0.1f);
    const Vec3 gravity = physics != nullptr ? physics->settings().gravity : Vec3{0.0f, -9.81f, 0.0f};

    // Emisores que ya no existen (o fuera de la vista previa).
    for (auto it = emitters_.begin(); it != emitters_.end();) {
        const entt::entity handle = it->first;
        const bool keep = world.valid(handle) && world.registry().all_of<ParticleSystem>(handle) &&
                          (only.empty() || std::find(only.begin(), only.end(), handle) != only.end());
        it = keep ? std::next(it) : emitters_.erase(it);
    }

    for (const entt::entity handle : world.registry().view<ParticleSystem>()) {
        if (!only.empty() && std::find(only.begin(), only.end(), handle) == only.end()) continue;
        const ecs::Entity entity = world.wrap(handle);
        if (!entity.activeInHierarchy()) {
            emitters_.erase(handle);
            continue;
        }
        const ParticleSystem& s = entity.get<ParticleSystem>();
        Emitter& em = emitters_[handle];
        if (!em.started) {
            em.started = true;
            em.playing = s.play_on_start || !only.empty();
        }
        em.matrix = entity.worldMatrix();
        em.blend = s.blend;
        em.size_start = s.size_start;
        em.size_end = s.size_end;
        em.color_start = colorOf(s.color_start, s.alpha_start, s.intensity);
        em.color_end = colorOf(s.color_end, s.alpha_end, s.intensity);
        if (em.world_space != s.world_space) {
            em.particles.clear();  // cambiar de espacio a mitad no tiene sentido
            em.world_space = s.world_space;
        }

        // --- Emision ---
        if (em.playing && dt > 0.0f) {
            if (!em.burst_done) {
                emit(em, s, s.burst);
                em.burst_done = true;
            }
            em.pending += std::max(s.rate, 0.0f) * dt;
            const int count = static_cast<int>(em.pending);
            em.pending -= static_cast<float>(count);
            emit(em, s, count);
            em.time += dt;
            if (em.time >= std::max(s.duration, 0.05f)) {
                if (s.looping) {
                    em.time -= std::max(s.duration, 0.05f);
                    em.burst_done = false;
                } else {
                    em.playing = false;
                }
            }
        }

        // --- Movimiento y colision ---
        const core::Mat4 inverse = em.world_space ? core::Mat4::identity() : core::inverse(em.matrix);
        const Vec3 local_gravity = em.world_space ? gravity : ecs::transformDirection(inverse, gravity);
        const Vec3 accel = local_gravity * s.gravity_modifier;
        const float drag = std::max(0.0f, 1.0f - std::max(s.drag, 0.0f) * dt);
        QueryFilter filter;
        filter.layer_mask = s.collides_with;
        filter.triggers = QueryTriggers::Ignore;
        filter.record = false;  // miles de rayos: no son para los gizmos
        const bool collide = s.collision && physics != nullptr && physics->running();
        int events_this_frame = 0;

        for (std::size_t i = 0; i < em.particles.size();) {
            Particle& p = em.particles[i];
            p.age += dt;
            if (p.age >= p.lifetime) {
                p = em.particles.back();
                em.particles.pop_back();
                continue;
            }
            p.velocity = (p.velocity + accel * dt) * drag;
            Vec3 next = p.position + p.velocity * dt;
            if (collide) {
                const float t = p.age / p.lifetime;
                const float radius =
                    std::max(0.5f * (s.size_start + (s.size_end - s.size_start) * t) * s.radius_scale, 0.001f);
                const Vec3 from = em.world_space ? p.position : ecs::transformPoint(em.matrix, p.position);
                const Vec3 to = em.world_space ? next : ecs::transformPoint(em.matrix, next);
                const Vec3 delta = to - from;
                const float travel = core::length(delta);
                RaycastHit hit;
                if (travel > 1e-6f && physics->raycast(from, delta, travel + radius, hit, filter)) {
                    const Vec3 n = hit.normal;
                    Vec3 velocity = em.world_space ? p.velocity : ecs::transformDirection(em.matrix, p.velocity);
                    const float vn_speed = core::dot(velocity, n);
                    if (vn_speed < 0.0f) {
                        const Vec3 vn = n * vn_speed;
                        const Vec3 vt = velocity - vn;
                        velocity = vt * (1.0f - std::clamp(s.dampen, 0.0f, 1.0f)) -
                                   vn * std::clamp(s.bounce, 0.0f, 1.0f);
                        p.age += std::clamp(s.lifetime_loss, 0.0f, 1.0f) * p.lifetime;
                        ++collisions_;
                        // Solo choques de verdad (no las que reposan en el suelo)
                        // y con tope por frame.
                        if (s.send_collision_events && -vn_speed > 0.25f && events_this_frame < 64) {
                            PhysicsEvent event;
                            event.type = PhysicsEventType::ParticleCollision;
                            event.a = entity;
                            event.b = hit.entity;
                            event.point = hit.point;
                            event.normal = n;
                            event.relative_velocity = -(em.world_space ? p.velocity
                                                                        : ecs::transformDirection(em.matrix, p.velocity));
                            event.contact_count = 1;
                            physics->reportEvent(event);
                            ++events_this_frame;
                        }
                    }
                    const Vec3 rest = hit.point + n * radius;
                    next = em.world_space ? rest : ecs::transformPoint(inverse, rest);
                    p.velocity = em.world_space ? velocity : ecs::transformDirection(inverse, velocity);
                }
            }
            p.position = next;
            ++i;
        }
    }
}

void ParticleWorld::shiftOrigin(const core::Vec3& offset) {
    for (auto& [handle, emitter] : emitters_) {
        emitter.matrix.m[3][0] -= offset.x;
        emitter.matrix.m[3][1] -= offset.y;
        emitter.matrix.m[3][2] -= offset.z;
        if (!emitter.world_space) continue;  // en local: siguen al emisor
        for (Particle& p : emitter.particles) p.position = p.position - offset;
    }
}

void ParticleWorld::clear() {
    emitters_.clear();
}

void ParticleWorld::play(ecs::Entity entity) {
    Emitter& em = emitters_[entity.handle()];
    em.started = true;
    em.playing = true;
    em.time = 0.0f;
    em.pending = 0.0f;
    em.burst_done = false;
}

void ParticleWorld::stop(ecs::Entity entity, bool clear_particles) {
    const auto it = emitters_.find(entity.handle());
    if (it == emitters_.end()) return;
    it->second.playing = false;
    if (clear_particles) it->second.particles.clear();
}

bool ParticleWorld::isPlaying(ecs::Entity entity) const {
    const auto it = emitters_.find(entity.handle());
    return it != emitters_.end() && it->second.playing;
}

gfx::ParticleDrawList ParticleWorld::drawList(const Vec3& camera_position) const {
    gfx::ParticleDrawList list;
    std::vector<std::pair<float, gfx::ParticleInstance>> sorted;
    for (const auto& [handle, em] : emitters_) {
        for (const Particle& p : em.particles) {
            const float t = std::clamp(p.age / p.lifetime, 0.0f, 1.0f);
            gfx::ParticleInstance instance;
            instance.position = em.world_space ? p.position : ecs::transformPoint(em.matrix, p.position);
            instance.size = em.size_start + (em.size_end - em.size_start) * t;
            instance.color = lerp4(em.color_start, em.color_end, t);
            if (em.blend == ParticleBlend::Additive) {
                list.additive.push_back(instance);
            } else {
                const Vec3 d = instance.position - camera_position;
                sorted.emplace_back(core::dot(d, d), instance);
            }
        }
    }
    // Transparentes: de atras adelante.
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    list.alpha.reserve(sorted.size());
    for (const auto& [distance, instance] : sorted) list.alpha.push_back(instance);
    return list;
}

std::size_t ParticleWorld::particleCount() const {
    std::size_t total = 0;
    for (const auto& [handle, em] : emitters_) total += em.particles.size();
    return total;
}

std::size_t ParticleWorld::particleCount(ecs::Entity entity) const {
    const auto it = emitters_.find(entity.handle());
    return it != emitters_.end() ? it->second.particles.size() : 0;
}

}  // namespace cramion::physics
