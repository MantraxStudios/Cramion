#include "CramionCore/physics/Destruction.h"

#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/RuntimeMesh.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"

#include <CramionFX/asset/Model.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace cramion::physics {

// Fracture.cpp: color lineal del modelo -> sRGB del Inspector.
core::Vec4 fractureSrgbColor(const core::Vec4& linear);

using core::Vec3;

void Destructible::reflect(ecs::PropertyVisitor& v) {
    v.asset({"fracture", "Fractura (.crfracture)", "Trozos precalculados (clic derecho en un modelo > Crear fractura)"},
            fracture, assets::AssetType::Fracture);
    if (v.beginGroup("Vida y golpes", true)) {
        v.field({"health", "Vida", "Se rompe al llegar a 0"}, health, ecs::FloatRange{0.0f, 100000.0f, 1.0f, "%.0f"});
        v.field({"break_on_impact", "Romper con golpes", "Los choques fuertes le quitan vida"}, break_on_impact);
        v.field({"damage_threshold", "Umbral de golpe", "m/s de choque que no hacen dano"}, damage_threshold,
                ecs::FloatRange{0.0f, 100.0f, 0.1f, "%.1f m/s"});
        v.field({"damage_per_speed", "Daño por m/s", "Dano por cada m/s por encima del umbral"}, damage_per_speed,
                ecs::FloatRange{0.0f, 10000.0f, 0.5f, "%.1f"});
        v.field({"break_force", "Fuerza de rotura", "m/s con que salen los trozos desde el golpe"}, break_force,
                ecs::FloatRange{0.0f, 100.0f, 0.1f, "%.1f m/s"});
        v.endGroup();
    }
    if (v.beginGroup("Trozos", true)) {
        v.asset({"interior_material", "Material del interior", "Material (.crmat) de las caras del corte"},
                interior_material, assets::AssetType::Material);
        v.field({"interior_color", "Color del interior", "Si no hay material"}, interior_color, ecs::Vec3Kind::Color);
        v.field({"density", "Densidad", "kg/m3 si el objeto no tiene Rigidbody (madera 600, piedra 2500)"}, density,
                ecs::FloatRange{1.0f, 20000.0f, 10.0f, "%.0f kg/m3"});
        v.field({"multi_level", "Romper otra vez", "Los trozos grandes (fractura de varios niveles) se rompen otra vez"},
                multi_level);
        v.field({"piece_health", "Vida de los trozos"}, piece_health, ecs::FloatRange{0.0f, 100000.0f, 1.0f, "%.0f"});
        v.endGroup();
    }
    if (v.beginGroup("Escombros", true)) {
        v.field({"debris_lifetime", "Duración", "Segundos hasta que desaparecen (0 = siempre)"}, debris_lifetime,
                ecs::FloatRange{0.0f, 600.0f, 0.1f, "%.1f s"});
        v.field({"fade_time", "Encoger durante", "Segundos encogiendo antes de desaparecer"}, fade_time,
                ecs::FloatRange{0.0f, 30.0f, 0.05f, "%.2f s"});
        v.field({"max_active_pieces", "Máximo de trozos", "En toda la escena: los mas viejos desaparecen"},
                max_active_pieces, 1, 5000);
        v.endGroup();
    }
}

void registerDestructionComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("Destructible") == nullptr) {
        registry.registerComponent<Destructible>("Destructible", "Destructible", "Fisica");
    }
}

// --- Sistema -------------------------------------------------------------------------

void DestructionSystem::reset() {
    breakables_.clear();
    health_.clear();
    broken_.clear();
    debris_.clear();
    requests_.clear();
}

std::shared_ptr<const FractureData> DestructionSystem::load(const Uuid& uuid) {
    if (!uuid.valid()) return nullptr;
    const auto it = cache_.find(uuid);
    if (it != cache_.end()) return it->second;
    std::shared_ptr<FractureData> data;
    if (assets_ != nullptr) {
        const auto info = assets_->database().find(uuid);
        if (info && !info->path.empty()) {
            auto loaded = std::make_shared<FractureData>();
            std::string error;
            const std::filesystem::path file =
                info->path.is_absolute() ? info->path : assets_->database().root() / info->path;
            if (loadFracture(file, *loaded, &error)) {
                data = std::move(loaded);
            } else {
                std::cerr << "[Destruccion] " << error << "\n";
            }
        }
    }
    cache_[uuid] = data;
    return data;
}

bool DestructionSystem::fracture(ecs::Entity entity, const Vec3& point, float force) {
    if (!entity.valid()) return false;
    if (!entity.has<Destructible>() && breakables_.find(entity.handle()) == breakables_.end()) return false;
    requests_.push_back(Request{entity.handle(), point, force});
    return true;
}

void DestructionSystem::applyDamage(ecs::Entity entity, float damage, const Vec3& point, float force) {
    if (!entity.valid() || damage <= 0.0f) return;
    const auto piece = breakables_.find(entity.handle());
    if (piece != breakables_.end()) {
        piece->second.health -= damage;
        if (piece->second.health <= 0.0f) requests_.push_back(Request{entity.handle(), point, force});
        return;
    }
    if (!entity.has<Destructible>()) return;
    auto [it, inserted] = health_.try_emplace(entity.handle(), entity.get<Destructible>().health);
    it->second -= damage;
    if (it->second <= 0.0f) requests_.push_back(Request{entity.handle(), point, force});
}

float DestructionSystem::health(ecs::Entity entity) const {
    if (!entity.valid()) return 0.0f;
    const auto piece = breakables_.find(entity.handle());
    if (piece != breakables_.end()) return piece->second.health;
    const auto it = health_.find(entity.handle());
    if (it != health_.end()) return it->second;
    return entity.has<Destructible>() ? entity.get<Destructible>().health : 0.0f;
}

bool DestructionSystem::isBroken(ecs::Entity entity) const {
    return entity.valid() && broken_.find(entity.handle()) != broken_.end();
}

int DestructionSystem::addBreakListener(BreakListener listener) {
    const int id = next_listener_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

void DestructionSystem::removeBreakListener(int id) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [id](const auto& l) { return l.first == id; }),
                     listeners_.end());
}

void DestructionSystem::update(ecs::World& world, PhysicsSystem& physics, float delta_seconds) {
    entt::registry& registry = world.registry();

    // Golpes: choques fuertes contra objetos rompibles.
    for (const PhysicsEvent& event : physics.events()) {
        if (event.type != PhysicsEventType::CollisionEnter) continue;
        const float speed = core::length(event.relative_velocity);
        for (int side = 0; side < 2; ++side) {
            const ecs::Entity e = side == 0 ? event.a : event.b;
            if (!e.valid() || broken_.count(e.handle())) continue;
            const Destructible* settings = nullptr;
            const auto piece = breakables_.find(e.handle());
            if (piece != breakables_.end()) {
                settings = &piece->second.settings;
            } else if (e.has<Destructible>()) {
                settings = &e.get<Destructible>();
            }
            if (settings == nullptr || !settings->break_on_impact) continue;
            if (speed <= settings->damage_threshold) continue;
            applyDamage(e, (speed - settings->damage_threshold) * settings->damage_per_speed, event.point, -1.0f);
        }
    }

    // Roturas pendientes (de golpes o de scripts).
    std::vector<Request> requests;
    requests.swap(requests_);
    for (const Request& r : requests) {
        if (!registry.valid(r.handle) || broken_.count(r.handle)) continue;
        breakEntity(world, physics, world.wrap(r.handle), r.point, r.force);
    }

    // Escombros: envejecen, encogen y desaparecen.
    for (auto it = debris_.begin(); it != debris_.end();) {
        if (!registry.valid(it->handle)) {
            breakables_.erase(it->handle);
            it = debris_.erase(it);
            continue;
        }
        if (it->lifetime <= 0.0f) {
            ++it;
            continue;
        }
        it->age += delta_seconds;
        ecs::Entity e = world.wrap(it->handle);
        if (it->age >= it->lifetime) {
            breakables_.erase(it->handle);
            world.destroy(e);
            it = debris_.erase(it);
            continue;
        }
        const float remaining = it->lifetime - it->age;
        if (remaining < it->fade && it->fade > 0.0f) {
            const float s = std::max(remaining / it->fade, 0.02f);
            e.setLocalScale(it->scale * s);
        }
        ++it;
    }
}

void DestructionSystem::breakEntity(ecs::World& world, PhysicsSystem& physics, ecs::Entity entity, const Vec3& point,
                                    float force) {
    std::vector<ecs::Entity> spawned;
    const Vec3 velocity = physics.hasBody(entity) ? physics.linearVelocity(entity) : Vec3{};
    const auto piece_it = breakables_.find(entity.handle());
    if (piece_it != breakables_.end()) {
        // Un trozo que se rompe en sus hijos.
        const Breakable b = piece_it->second;
        breakables_.erase(piece_it);
        if (!b.data || b.piece < 0 || b.piece >= static_cast<int>(b.data->pieces.size())) return;
        const FracturePiece& piece = b.data->pieces[static_cast<std::size_t>(b.piece)];
        if (piece.children.empty()) return;
        const core::Mat4 frame = entity.worldMatrix() * core::translate(Vec3{-piece.center.x, -piece.center.y, -piece.center.z});
        const Rigidbody* body = entity.tryGet<Rigidbody>();
        const float mass = body != nullptr ? body->mass : 1.0f;
        // La masa del trozo se reparte entre sus hijos (por volumen).
        const float scale_volume = piece.volume > 1e-9f ? mass / piece.volume : 1.0f;
        spawnPieces(world, physics, entity, *b.data, piece.children, frame, velocity,
                    scale_volume * b.data->total_volume, b.settings, point,
                    force >= 0.0f ? force : b.settings.break_force, spawned);
        broken_[entity.handle()] = true;
        BreakEvent event{entity, point, force >= 0.0f ? force : b.settings.break_force, spawned};
        for (auto& [id, listener] : listeners_) listener(event);
        // El trozo roto desaparece (sus hijos ocupan su sitio).
        for (auto it = debris_.begin(); it != debris_.end(); ++it) {
            if (it->handle == entity.handle()) {
                debris_.erase(it);
                break;
            }
        }
        world.destroy(entity);
        return;
    }

    if (!entity.has<Destructible>()) return;
    const Destructible settings = entity.get<Destructible>();
    const std::shared_ptr<const FractureData> data = load(settings.fracture.uuid);
    if (!data || data->pieces.empty()) {
        std::cerr << "[Destruccion] " << entity.name() << ": sin fractura (.crfracture) valida\n";
        broken_[entity.handle()] = true;
        return;
    }
    // Masa total: la del Rigidbody o la densidad por el volumen (con escala).
    float mass = 0.0f;
    if (const Rigidbody* body = entity.tryGet<Rigidbody>(); body != nullptr && body->type == BodyType::Dynamic) {
        mass = body->mass;
    } else {
        Vec3 position, scale;
        core::Quat rotation;
        ecs::decomposeMatrix(entity.worldMatrix(), position, rotation, scale);
        mass = settings.density * data->total_volume * std::fabs(scale.x * scale.y * scale.z);
    }
    mass = std::max(mass, 0.1f);
    spawnPieces(world, physics, entity, *data, data->roots(), entity.worldMatrix(), velocity, mass, settings, point,
                force >= 0.0f ? force : settings.break_force, spawned);
    broken_[entity.handle()] = true;
    entity.setActive(false);  // el objeto entero se oculta (sigue existiendo para los scripts)
    BreakEvent event{entity, point, force >= 0.0f ? force : settings.break_force, spawned};
    for (auto& [id, listener] : listeners_) listener(event);
}

void DestructionSystem::spawnPieces(ecs::World& world, PhysicsSystem& /*physics*/, ecs::Entity source,
                                    const FractureData& data, const std::vector<int>& pieces, const core::Mat4& frame,
                                    const Vec3& velocity, float mass, const Destructible& settings, const Vec3& point,
                                    float force, std::vector<ecs::Entity>& spawned) {
    // Materiales: el de fuera del objeto original, el de dentro del componente.
    ecs::MeshMaterial exterior;
    assets::AssetRef exterior_override{{}, assets::AssetType::Material};
    if (const ecs::MeshRenderer* renderer = source.tryGet<ecs::MeshRenderer>()) {
        if (!renderer->materials.empty()) exterior_override = renderer->materials.front();
        if (renderer->mesh && !renderer->mesh->materials.empty()) {
            exterior = renderer->mesh->materials.front();
        } else if (assets_ != nullptr && renderer->model.valid()) {
            if (const auto model = assets_->loadModel(renderer->model.uuid)) {
                const int part = std::clamp(renderer->part, 0, std::max(0, static_cast<int>(model->parts.size()) - 1));
                if (!model->parts.empty() && model->parts[static_cast<std::size_t>(part)] &&
                    !model->parts[static_cast<std::size_t>(part)]->materials.empty()) {
                    const asset::MaterialData& m = model->parts[static_cast<std::size_t>(part)]->materials.front();
                    exterior.color = fractureSrgbColor(m.base_color);
                    exterior.metallic = m.metallic;
                    exterior.roughness = m.roughness;
                }
            }
        }
    }
    ecs::MeshMaterial interior;
    interior.color = core::Vec4{settings.interior_color, 1.0f};
    interior.roughness = 0.9f;

    const float total_volume = std::max(data.total_volume, 1e-6f);
    const std::shared_ptr<const FractureData> shared = cache_.count(settings.fracture.uuid) ? cache_[settings.fracture.uuid] : nullptr;
    const std::string base_name = source.name();
    int index = 0;
    for (const int p : pieces) {
        if (p < 0 || p >= static_cast<int>(data.pieces.size())) continue;
        const FracturePiece& piece = data.pieces[static_cast<std::size_t>(p)];
        if (piece.positions.empty() || (piece.exterior.empty() && piece.interior.empty())) continue;

        ecs::Entity e = world.create(base_name + " (trozo " + std::to_string(++index) + ")");
        const core::Mat4 world_matrix = frame * core::translate(piece.center);
        e.setWorldMatrix(world_matrix);
        Vec3 position, scale;
        core::Quat rotation;
        ecs::decomposeMatrix(world_matrix, position, rotation, scale);

        ecs::MeshRenderer& renderer = e.add<ecs::MeshRenderer>();
        renderer.mesh = pieceMesh(piece, exterior, interior);
        renderer.materials = {exterior_override, settings.interior_material};

        Rigidbody& body = e.add<Rigidbody>();
        body.type = BodyType::Dynamic;
        body.mass = std::max(mass * piece.volume / total_volume, 0.05f);
        // Hacia fuera desde el golpe (mas fuerte cerca).
        Vec3 away = position - point;
        const float distance = core::length(away);
        away = distance > 1e-4f ? away * (1.0f / distance) : Vec3{0.0f, 1.0f, 0.0f};
        const float falloff = 1.0f / (1.0f + distance);
        body.initial_velocity = velocity + away * (force * falloff);
        const float spin = force * falloff * 0.5f;
        body.initial_angular_velocity = Vec3{away.z * spin, 0.3f * spin, -away.x * spin};
        MeshCollider& collider = e.add<MeshCollider>();
        collider.convex = true;

        spawned.push_back(e);
        Debris d;
        d.handle = e.handle();
        d.lifetime = settings.debris_lifetime;
        d.fade = settings.fade_time;
        d.scale = scale;
        debris_.push_back(d);
        if (settings.multi_level && !piece.children.empty() && shared) {
            breakables_[e.handle()] = Breakable{shared, p, settings.piece_health, settings};
        }
    }
    // Maximo de trozos en la escena: los mas viejos se van.
    const std::size_t cap = static_cast<std::size_t>(std::max(settings.max_active_pieces, 1));
    while (debris_.size() > cap) {
        const entt::entity oldest = debris_.front().handle;
        debris_.pop_front();
        breakables_.erase(oldest);
        if (world.registry().valid(oldest)) world.destroy(world.wrap(oldest));
    }
}

}  // namespace cramion::physics
