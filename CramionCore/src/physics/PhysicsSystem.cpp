// PhysicsSystem sobre Jolt Physics.
//
// Capas: la capa de la entidad (0..31) va en los 5 bits bajos de la
// ObjectLayer de Jolt y el bit 5 marca los cuerpos que no se mueven. Hay dos
// capas de broadphase (quietos / moviles); la matriz de PhysicsSettings decide
// que pares de capas chocan.
//
// Cada entidad con colliders tiene hasta dos cuerpos: el solido (sus
// colliders normales) y el sensor (sus triggers). Si la entidad se mueve
// (dinamica o cinematica), el sensor es cinematico y la acompana.
//
// Los callbacks de contacto de Jolt llegan desde sus hilos: solo se apuntan
// (con un mutex) y tras cada paso, en el hilo de update(), se convierten en
// eventos Enter / Stay / Exit por pareja de cuerpos.

#include "CramionCore/physics/PhysicsSystem.h"

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/terrain/Terrain.h"

#include <Jolt/Jolt.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Geometry/Plane.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace cramion::physics {

using core::Quat;
using core::Vec3;

// -----------------------------------------------------------------------------
// Eventos
// -----------------------------------------------------------------------------

const char* eventTypeName(PhysicsEventType type) {
    switch (type) {
        case PhysicsEventType::CollisionEnter: return "CollisionEnter";
        case PhysicsEventType::CollisionStay: return "CollisionStay";
        case PhysicsEventType::CollisionExit: return "CollisionExit";
        case PhysicsEventType::TriggerEnter: return "TriggerEnter";
        case PhysicsEventType::TriggerStay: return "TriggerStay";
        case PhysicsEventType::TriggerExit: return "TriggerExit";
        case PhysicsEventType::ParticleCollision: return "ParticleCollision";
    }
    return "?";
}

bool isTriggerEvent(PhysicsEventType type) {
    return type == PhysicsEventType::TriggerEnter || type == PhysicsEventType::TriggerStay ||
           type == PhysicsEventType::TriggerExit;
}

PhysicsEvent PhysicsEvent::flipped() const {
    PhysicsEvent e = *this;
    std::swap(e.a, e.b);
    e.normal = -normal;
    e.relative_velocity = -relative_velocity;
    return e;
}

namespace {

// -----------------------------------------------------------------------------
// Jolt: arranque global, conversiones, capas y materiales
// -----------------------------------------------------------------------------

void joltTrace(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::cerr << "[Jolt] " << buffer << "\n";
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssert(const char* expression, const char* message, const char* file, JPH::uint line) {
    std::cerr << "[Jolt] Asercion: " << expression << " " << (message != nullptr ? message : "") << " ("
              << file << ":" << line << ")\n";
    return false;  // no parar en el depurador
}
#endif

#if defined(_DEBUG) && defined(_WIN32)
// En Debug el malloc del CRT (msvcrtd) comprueba el heap en cada llamada:
// con Jolt eso multiplicaba por mas de 10 el coste de cada paso. Jolt pide
// memoria directamente al heap de Windows (el mismo que usa Release).
void* joltAllocate(size_t size) {
    return HeapAlloc(GetProcessHeap(), 0, size);
}
void* joltReallocate(void* block, size_t /*old_size*/, size_t new_size) {
    return block == nullptr ? HeapAlloc(GetProcessHeap(), 0, new_size)
                            : HeapReAlloc(GetProcessHeap(), 0, block, new_size);
}
void joltFree(void* block) {
    if (block != nullptr) HeapFree(GetProcessHeap(), 0, block);
}
// Alineado: se reserva de mas y se guarda el puntero original justo antes.
void* joltAlignedAllocate(size_t size, size_t alignment) {
    void* raw = HeapAlloc(GetProcessHeap(), 0, size + alignment + sizeof(void*));
    if (raw == nullptr) return nullptr;
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
    const std::uintptr_t aligned = (start + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}
void joltAlignedFree(void* block) {
    if (block != nullptr) HeapFree(GetProcessHeap(), 0, reinterpret_cast<void**>(block)[-1]);
}
#endif

void ensureJolt() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
#if defined(_DEBUG) && defined(_WIN32)
        JPH::Allocate = joltAllocate;
        JPH::Reallocate = joltReallocate;
        JPH::Free = joltFree;
        JPH::AlignedAllocate = joltAlignedAllocate;
        JPH::AlignedFree = joltAlignedFree;
#endif
        JPH::Trace = joltTrace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = joltAssert;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

JPH::Vec3 toJolt(const Vec3& v) {
    return JPH::Vec3(v.x, v.y, v.z);
}
JPH::Quat toJolt(const Quat& q) {
    return JPH::Quat(q.x, q.y, q.z, q.w).Normalized();
}
Vec3 fromJolt(JPH::Vec3Arg v) {
    return Vec3{v.GetX(), v.GetY(), v.GetZ()};
}
Quat fromJolt(JPH::QuatArg q) {
    return Quat{q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
}

std::uint64_t entityToUserData(entt::entity entity) {
    return static_cast<std::uint64_t>(entt::to_integral(entity));
}
entt::entity userDataToEntity(std::uint64_t data) {
    return static_cast<entt::entity>(static_cast<std::underlying_type_t<entt::entity>>(data));
}

constexpr JPH::ObjectLayer kStaticFlag = 32;

JPH::ObjectLayer objectLayer(int layer, bool moving) {
    const auto l = static_cast<JPH::ObjectLayer>(std::clamp(layer, 0, kLayerCount - 1));
    return moving ? l : static_cast<JPH::ObjectLayer>(l | kStaticFlag);
}

namespace broadphase {
constexpr JPH::BroadPhaseLayer kNonMoving(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
constexpr JPH::uint kCount = 2;
}  // namespace broadphase

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return broadphase::kCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return (layer & kStaticFlag) != 0 ? broadphase::kNonMoving : broadphase::kMoving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == broadphase::kNonMoving ? "Quietos" : "Moviles";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
        // Lo quieto solo necesita mirar lo que se mueve.
        return (layer & kStaticFlag) == 0 || bp == broadphase::kMoving;
    }
};

class LayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    LayerPairFilter(const PhysicsSettings& settings, const std::array<std::uint32_t, kLayerCount>& includes)
        : settings_(settings), includes_(includes) {}
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if ((a & kStaticFlag) != 0 && (b & kStaticFlag) != 0) {
            return false;
        }
        // La matriz, o alguna capa anulada (Incluir) de algun cuerpo de esas
        // capas: la decision exacta, por cuerpo, en OnContactValidate.
        const int la = a & 31;
        const int lb = b & 31;
        return settings_.layersCollide(la, lb) || (includes_[static_cast<std::size_t>(la)] & layerBit(lb)) != 0;
    }

private:
    const PhysicsSettings& settings_;
    const std::array<std::uint32_t, kLayerCount>& includes_;
};

// Material de un collider (friccion y rebote por forma, no por cuerpo).
class ColliderPhysicsMaterial final : public JPH::PhysicsMaterial {
public:
    ColliderPhysicsMaterial(float friction, float restitution)
        : friction(friction), restitution(restitution) {}
    float friction;
    float restitution;
};

// Filtros de las consultas.
class MaskLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit MaskLayerFilter(std::uint32_t mask) : mask_(mask) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return (mask_ & (1u << (layer & 31))) != 0;
    }

private:
    std::uint32_t mask_;
};

class QueryBodyFilter final : public JPH::BodyFilter {
public:
    QueryBodyFilter(bool hit_triggers, JPH::BodyID ignore_a, JPH::BodyID ignore_b)
        : hit_triggers_(hit_triggers), ignore_a_(ignore_a), ignore_b_(ignore_b) {}
    bool ShouldCollide(const JPH::BodyID& id) const override {
        return id != ignore_a_ && id != ignore_b_;
    }
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        return hit_triggers_ || !body.IsSensor();
    }

private:
    bool hit_triggers_;
    JPH::BodyID ignore_a_;
    JPH::BodyID ignore_b_;
};

std::uint64_t pairKey(std::uint32_t a, std::uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<std::uint64_t>(a) << 32) | b;
}

bool nearlyEqual(const Vec3& a, const Vec3& b, float epsilon) {
    return std::abs(a.x - b.x) <= epsilon && std::abs(a.y - b.y) <= epsilon && std::abs(a.z - b.z) <= epsilon;
}

bool sameRotation(const Quat& a, const Quat& b) {
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    return std::abs(d) > 1.0f - 1e-6f;
}

bool sameScale(const Vec3& a, const Vec3& b) {
    const auto close = [](float x, float y) {
        return std::abs(x - y) <= 1e-4f * std::max(1.0f, std::max(std::abs(x), std::abs(y)));
    };
    return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z);
}

}  // namespace

// -----------------------------------------------------------------------------
// Estado
// -----------------------------------------------------------------------------

struct PhysicsSystem::Impl {
    // Contacto apuntado desde los hilos de Jolt.
    struct RawContact {
        enum class Kind { Added, Persisted, Removed } kind = Kind::Added;
        std::uint32_t body1 = 0;
        std::uint32_t body2 = 0;
        std::uint64_t sub_key = 0;
        entt::entity entity1 = entt::null;
        entt::entity entity2 = entt::null;
        bool sensor1 = false;
        bool sensor2 = false;
        Vec3 point{};
        Vec3 normal{};  // de 1 a 2
        Vec3 relative_velocity{};
        float penetration = 0.0f;
        int count = 0;
    };

    // Pareja de cuerpos en contacto (por sus subformas).
    struct Pair {
        entt::entity a = entt::null;  // en triggers, el sensor
        entt::entity b = entt::null;
        std::uint32_t body_a = 0;
        std::uint32_t body_b = 0;
        bool trigger = false;
        bool swapped = false;  // a es el cuerpo 2 del contacto (normales invertidas)
        std::unordered_set<std::uint64_t> subs;
        bool touched = false;  // Added o Persisted en este paso
        bool entered = false;  // Enter en este paso
        RawContact last{};
    };

    struct BodyEntry {
        entt::entity entity = entt::null;
        JPH::BodyID solid{};
        JPH::BodyID sensor{};
        BodyType type = BodyType::Static;
        std::uint64_t signature = 0;
        std::uint64_t terrain_key = 0;
        const void* mesh = nullptr;
        // Pose que tiene la entidad (la ultima que se leyo o se escribio).
        Vec3 position{};
        Quat rotation{};
        Vec3 scale{1.0f, 1.0f, 1.0f};
        core::Mat4 matrix{};  // su matriz de mundo entonces (para saltarla si no cambia)
        // Dinamicos: pose fisica antes y despues del ultimo paso (interpolacion).
        Vec3 previous_position{};
        Quat previous_rotation{};
        Vec3 current_position{};
        Quat current_rotation{};
        bool interpolate = true;
        int layer = 0;
        std::uint64_t seen = 0;  // ultima sincronizacion en que existia (las demas se borran)
    };

    struct MeshKey {
        const void* data = nullptr;
        bool convex = false;
        const JPH::PhysicsMaterial* material = nullptr;
        bool operator<(const MeshKey& o) const {
            return std::tie(data, convex, material) < std::tie(o.data, o.convex, o.material);
        }
    };

    class Listener final : public JPH::ContactListener {
    public:
        explicit Listener(Impl& impl) : impl_(impl) {}

        JPH::ValidateResult OnContactValidate(const JPH::Body& body1, const JPH::Body& body2, JPH::RVec3Arg,
                                              const JPH::CollideShapeResult&) override {
            // El solido y el sensor de la misma entidad no se detectan.
            if (body1.GetUserData() == body2.GetUserData()) {
                return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
            }
            return impl_.allowedByOverrides(body1, body2) ? JPH::ValidateResult::AcceptAllContactsForThisBodyPair
                                                          : JPH::ValidateResult::RejectAllContactsForThisBodyPair;
        }
        void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& settings) override {
            record(RawContact::Kind::Added, body1, body2, manifold, settings);
        }
        void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                                const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override {
            record(RawContact::Kind::Persisted, body1, body2, manifold, settings);
        }
        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
            RawContact c;
            c.kind = RawContact::Kind::Removed;
            c.body1 = pair.GetBody1ID().GetIndexAndSequenceNumber();
            c.body2 = pair.GetBody2ID().GetIndexAndSequenceNumber();
            c.sub_key = subKey(c.body1, c.body2, pair.GetSubShapeID1().GetValue(), pair.GetSubShapeID2().GetValue());
            std::lock_guard lock(impl_.contacts_mutex);
            impl_.raw_contacts.push_back(c);
        }

    private:
        static std::uint64_t subKey(std::uint32_t body1, std::uint32_t body2, std::uint32_t sub1, std::uint32_t sub2) {
            if (body1 > body2) std::swap(sub1, sub2);
            return (static_cast<std::uint64_t>(sub1) << 32) | sub2;
        }

        static void material(const JPH::Body& body, const JPH::SubShapeID& id, float& friction, float& restitution) {
            const JPH::PhysicsMaterial* m = body.GetShape()->GetMaterial(id);
            if (const auto* collider = dynamic_cast<const ColliderPhysicsMaterial*>(m)) {
                friction = collider->friction;
                restitution = collider->restitution;
            } else {
                friction = body.GetFriction();
                restitution = body.GetRestitution();
            }
        }

        void record(RawContact::Kind kind, const JPH::Body& body1, const JPH::Body& body2,
                    const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) {
            // Material por collider: friccion media (como Unity) y el rebote
            // mayor.
            float f1 = 0.0f, r1 = 0.0f, f2 = 0.0f, r2 = 0.0f;
            material(body1, manifold.mSubShapeID1, f1, r1);
            material(body2, manifold.mSubShapeID2, f2, r2);
            settings.mCombinedFriction = 0.5f * (f1 + f2);
            settings.mCombinedRestitution = std::max(r1, r2);

            RawContact c;
            c.kind = kind;
            c.body1 = body1.GetID().GetIndexAndSequenceNumber();
            c.body2 = body2.GetID().GetIndexAndSequenceNumber();
            c.sub_key = subKey(c.body1, c.body2, manifold.mSubShapeID1.GetValue(), manifold.mSubShapeID2.GetValue());
            c.entity1 = userDataToEntity(body1.GetUserData());
            c.entity2 = userDataToEntity(body2.GetUserData());
            c.sensor1 = body1.IsSensor();
            c.sensor2 = body2.IsSensor();
            const JPH::uint points = manifold.mRelativeContactPointsOn1.size();
            JPH::RVec3 sum = JPH::RVec3::sZero();
            for (JPH::uint i = 0; i < points; ++i) {
                sum += 0.5f * (manifold.GetWorldSpaceContactPointOn1(i) + manifold.GetWorldSpaceContactPointOn2(i));
            }
            const JPH::RVec3 point = points > 0 ? sum / static_cast<float>(points) : manifold.mBaseOffset;
            c.point = fromJolt(JPH::Vec3(point));
            c.normal = fromJolt(manifold.mWorldSpaceNormal);
            c.relative_velocity = fromJolt(body2.GetPointVelocity(point) - body1.GetPointVelocity(point));
            c.penetration = manifold.mPenetrationDepth;
            c.count = static_cast<int>(points);
            std::lock_guard lock(impl_.contacts_mutex);
            impl_.raw_contacts.push_back(c);
        }

        Impl& impl_;
    };

    Impl() : layer_filter(settings, layer_includes), listener(*this) {}

    // Capas anuladas por cuerpo (solo los que tienen alguna). Se leen desde
    // los hilos de Jolt durante Update: solo se cambian en sync (fuera).
    struct LayerOverride {
        std::uint32_t include = 0;
        std::uint32_t exclude = 0;
    };
    std::unordered_map<std::uint32_t, LayerOverride> layer_overrides;
    std::array<std::uint32_t, kLayerCount> layer_includes{};  // union de Incluir por capa (simetrica)
    bool overrides_dirty = false;

    bool allowedByOverrides(const JPH::Body& body1, const JPH::Body& body2) const {
        if (layer_overrides.empty()) return true;  // ya lo decidio la matriz
        const int la = body1.GetObjectLayer() & 31;
        const int lb = body2.GetObjectLayer() & 31;
        const auto o1 = layer_overrides.find(body1.GetID().GetIndexAndSequenceNumber());
        const auto o2 = layer_overrides.find(body2.GetID().GetIndexAndSequenceNumber());
        const LayerOverride a = o1 != layer_overrides.end() ? o1->second : LayerOverride{};
        const LayerOverride b = o2 != layer_overrides.end() ? o2->second : LayerOverride{};
        if ((a.exclude & layerBit(lb)) != 0 || (b.exclude & layerBit(la)) != 0) return false;
        if ((a.include & layerBit(lb)) != 0 || (b.include & layerBit(la)) != 0) return true;
        return settings.layersCollide(la, lb);
    }

    void rebuildLayerIncludes() {
        layer_includes.fill(0);
        for (const auto& [body, o] : layer_overrides) {
            if (o.include == 0) continue;
            const auto it = body_entities.find(body);
            if (it == body_entities.end()) continue;
            const auto entry = entries.find(it->second);
            if (entry == entries.end()) continue;
            const int layer = entry->second.layer;
            layer_includes[static_cast<std::size_t>(layer)] |= o.include;
            for (int j = 0; j < kLayerCount; ++j) {
                if ((o.include & layerBit(j)) != 0) layer_includes[static_cast<std::size_t>(j)] |= layerBit(layer);
            }
        }
        overrides_dirty = false;
    }

    PhysicsSettings settings;
    BroadPhaseLayers broadphase_layers;
    ObjectVsBroadPhase object_vs_broadphase;
    LayerPairFilter layer_filter;
    Listener listener;

    // Bloque fijo y, si una escena enorme no cabe, memoria del heap.
    std::unique_ptr<JPH::TempAllocatorImplWithMallocFallback> temp_allocator;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system;
    std::unique_ptr<JPH::PhysicsSystem> system;

    MeshProvider mesh_provider;
    TerrainProvider terrain_provider;
    assets::AssetManager* asset_manager = nullptr;

    std::unordered_map<entt::entity, BodyEntry> entries;
    std::unordered_map<std::uint32_t, entt::entity> body_entities;
    std::map<MeshKey, JPH::RefConst<JPH::Shape>> mesh_shapes;
    std::map<std::pair<int, int>, JPH::RefConst<JPH::PhysicsMaterial>> materials;
    std::unordered_set<entt::entity> warned;

    std::mutex contacts_mutex;
    std::vector<RawContact> raw_contacts;
    std::unordered_map<std::uint64_t, Pair> pairs;

    ecs::World* world = nullptr;
    std::vector<entt::entity> sync_candidates;
    std::uint64_t sync_generation = 0;
    float accumulator = 0.0f;
    std::uint64_t steps = 0;
    float step_ms = 0.0f;
    std::vector<PhysicsEvent> events;
    std::vector<ContactDebug> contact_points;

    struct ListenerEntry {
        int id = 0;
        entt::entity entity = entt::null;  // null = todos los eventos
        std::shared_ptr<EventCallback> callback;  // null = quitado durante un evento
    };
    int dispatching = 0;
    std::vector<ListenerEntry> listeners;
    int next_listener = 1;

    mutable std::vector<QueryDebug> queries;
    bool record_queries = false;

    // --- Utilidades ---
    JPH::BodyInterface& bodies() { return system->GetBodyInterface(); }
    const JPH::BodyInterface& bodies() const { return system->GetBodyInterface(); }

    const BodyEntry* entryOf(ecs::Entity entity) const {
        if (!system || !entity.valid()) return nullptr;
        const auto it = entries.find(entity.handle());
        return it != entries.end() ? &it->second : nullptr;
    }

    const JPH::PhysicsMaterial* materialFor(const ColliderMaterial& m) {
        const std::pair<int, int> key{static_cast<int>(std::lround(m.friction * 1000.0f)),
                                      static_cast<int>(std::lround(m.bounciness * 1000.0f))};
        auto& slot = materials[key];
        if (slot == nullptr) {
            slot = new ColliderPhysicsMaterial(std::max(m.friction, 0.0f), std::clamp(m.bounciness, 0.0f, 1.0f));
        }
        return slot.GetPtr();
    }

    void warnOnce(entt::entity entity, const std::string& name, const char* message) {
        if (warned.insert(entity).second) {
            std::cerr << "[Fisica] " << name << ": " << message << "\n";
        }
    }

    bool hitTriggers(const QueryFilter& filter) const {
        return filter.triggers == QueryTriggers::UseGlobal ? settings.queries_hit_triggers
                                                           : filter.triggers == QueryTriggers::Collide;
    }

    ecs::Entity entityOfBody(const JPH::BodyID& id) const {
        const auto it = body_entities.find(id.GetIndexAndSequenceNumber());
        return it != body_entities.end() && world != nullptr ? world->wrap(it->second) : ecs::Entity{};
    }

    void applySettings() {
        if (!system) return;
        system->SetGravity(toJolt(settings.gravity));
        JPH::PhysicsSettings ps = system->GetPhysicsSettings();
        ps.mPointVelocitySleepThreshold = std::max(settings.sleep_threshold, 1e-4f);
        system->SetPhysicsSettings(ps);
    }

    // Sin copiar la lista por evento (miles por paso con muchos contactos):
    // se recorre por indice; un oyente anadido durante el evento entra al
    // final y uno quitado queda vacio hasta que termina.
    void dispatch(const PhysicsEvent& event) {
        events.push_back(event);
        ++dispatching;
        const std::size_t count = listeners.size();
        for (std::size_t i = 0; i < count; ++i) {
            const entt::entity target = listeners[i].entity;
            const std::shared_ptr<EventCallback> callback = listeners[i].callback;
            if (!callback) continue;
            if (target == entt::null || event.a.handle() == target) {
                (*callback)(event);
            } else if (event.b.handle() == target) {
                (*callback)(event.flipped());
            }
        }
        if (--dispatching == 0) {
            listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
                                           [](const ListenerEntry& l) { return !l.callback; }),
                            listeners.end());
        }
    }

    // --- Formas ---

    struct ShapePart {
        JPH::RefConst<JPH::Shape> shape;
        Vec3 position{};
        Quat rotation{};
    };

    const asset::ModelData* meshOf(ecs::Entity entity) const {
        const ecs::MeshRenderer* renderer = entity.tryGet<ecs::MeshRenderer>();
        if (renderer == nullptr || !renderer->model.valid()) return nullptr;
        if (mesh_provider) {
            if (const asset::ModelData* data = mesh_provider(entity)) return data;
        }
        if (asset_manager != nullptr) {
            const auto model = asset_manager->loadModel(renderer->model.uuid);
            if (model && renderer->part >= 0 && static_cast<std::size_t>(renderer->part) < model->parts.size()) {
                return model->parts[static_cast<std::size_t>(renderer->part)].get();
            }
        }
        return nullptr;
    }

    JPH::RefConst<JPH::Shape> meshShape(const asset::ModelData& data, bool convex,
                                        const JPH::PhysicsMaterial* material, const std::string& name) {
        auto& slot = mesh_shapes[MeshKey{&data, convex, material}];
        if (slot != nullptr) return slot;
        if (data.vertices.empty()) return nullptr;

        if (!convex && data.indices.size() >= 3) {
            JPH::VertexList vertices;
            vertices.reserve(data.vertices.size());
            for (const asset::SkinnedVertex& v : data.vertices) {
                vertices.push_back(JPH::Float3(v.position.x, v.position.y, v.position.z));
            }
            JPH::IndexedTriangleList triangles;
            triangles.reserve(data.indices.size() / 3);
            for (std::size_t i = 0; i + 2 < data.indices.size(); i += 3) {
                std::uint32_t i0 = data.indices[i], i1 = data.indices[i + 1], i2 = data.indices[i + 2];
                if (i0 >= data.vertices.size() || i1 >= data.vertices.size() || i2 >= data.vertices.size()) continue;
                // La cara de delante de Jolt es la antihoraria respecto a su
                // normal: se orienta cada triangulo con las normales de la
                // malla (los modelos importados no siempre coinciden).
                const Vec3& p0 = data.vertices[i0].position;
                const Vec3 face = core::cross(data.vertices[i1].position - p0, data.vertices[i2].position - p0);
                const Vec3 normal = data.vertices[i0].normal + data.vertices[i1].normal + data.vertices[i2].normal;
                if (core::dot(face, normal) < 0.0f) std::swap(i1, i2);
                triangles.push_back(JPH::IndexedTriangle(i0, i1, i2, 0));
            }
            JPH::PhysicsMaterialList materials_list;
            materials_list.push_back(material);
            JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles), std::move(materials_list));
            const JPH::ShapeSettings::ShapeResult result = settings.Create();
            if (!result.HasError()) {
                slot = result.Get();
                return slot;
            }
            std::cerr << "[Fisica] " << name << ": malla no valida (" << result.GetError().c_str()
                      << "), se usa su envolvente convexa\n";
        }

        // Envolvente convexa (como mucho ~4000 puntos: se submuestrea).
        JPH::Array<JPH::Vec3> points;
        const std::size_t stride = std::max<std::size_t>(1, data.vertices.size() / 4000);
        Vec3 lo{1e30f, 1e30f, 1e30f};
        Vec3 hi{-1e30f, -1e30f, -1e30f};
        for (std::size_t i = 0; i < data.vertices.size(); ++i) {
            const Vec3& p = data.vertices[i].position;
            lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            if (i % stride == 0) points.push_back(toJolt(p));
        }
        JPH::ConvexHullShapeSettings hull(points, JPH::cDefaultConvexRadius, material);
        const JPH::ShapeSettings::ShapeResult result = hull.Create();
        if (!result.HasError()) {
            slot = result.Get();
            return slot;
        }
        // Malla plana o degenerada: su caja.
        const Vec3 half{std::max((hi.x - lo.x) * 0.5f, 0.01f), std::max((hi.y - lo.y) * 0.5f, 0.01f),
                        std::max((hi.z - lo.z) * 0.5f, 0.01f)};
        const Vec3 center = (lo + hi) * 0.5f;
        const float radius = std::min({0.05f, half.x * 0.5f, half.y * 0.5f, half.z * 0.5f});
        const JPH::RefConst<JPH::Shape> box = new JPH::BoxShape(toJolt(half), radius, material);
        slot = JPH::RotatedTranslatedShapeSettings(toJolt(center), JPH::Quat::sIdentity(), box).Create().Get();
        return slot;
    }

    // Formas de los colliders de la entidad (en su espacio local, sin escala),
    // separadas en solidas y triggers.
    void collectShapes(ecs::Entity entity, BodyType type, std::vector<ShapePart>& solid,
                       std::vector<ShapePart>& sensor, const void*& mesh_used) {
        const std::string& name = entity.name();
        const auto add = [&](const ColliderMaterial& m, JPH::RefConst<JPH::Shape> shape, const Vec3& position,
                             const Quat& rotation) {
            if (shape == nullptr) return;
            (m.is_trigger ? sensor : solid).push_back(ShapePart{std::move(shape), position, rotation});
        };
        if (const BoxCollider* box = entity.tryGet<BoxCollider>()) {
            const Vec3 half{std::max(std::abs(box->size.x) * 0.5f, 1e-3f), std::max(std::abs(box->size.y) * 0.5f, 1e-3f),
                            std::max(std::abs(box->size.z) * 0.5f, 1e-3f)};
            const float radius = std::min({JPH::cDefaultConvexRadius, half.x * 0.5f, half.y * 0.5f, half.z * 0.5f});
            add(box->material, new JPH::BoxShape(toJolt(half), radius, materialFor(box->material)), box->center, Quat{});
        }
        if (const SphereCollider* sphere = entity.tryGet<SphereCollider>()) {
            add(sphere->material, new JPH::SphereShape(std::max(sphere->radius, 1e-3f), materialFor(sphere->material)),
                sphere->center, Quat{});
        }
        if (const CapsuleCollider* capsule = entity.tryGet<CapsuleCollider>()) {
            const float radius = std::max(capsule->radius, 1e-3f);
            const float half_height = std::max(capsule->height * 0.5f - radius, 0.0f);
            const JPH::PhysicsMaterial* material = materialFor(capsule->material);
            JPH::RefConst<JPH::Shape> shape;
            if (half_height < 1e-3f) {
                shape = new JPH::SphereShape(radius, material);
            } else {
                shape = new JPH::CapsuleShape(half_height, radius, material);
            }
            // La capsula de Jolt va a lo largo de Y.
            Quat rotation{};
            if (capsule->axis == CapsuleAxis::X) {
                rotation = fromJolt(JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), -0.5f * JPH::JPH_PI));
            } else if (capsule->axis == CapsuleAxis::Z) {
                rotation = fromJolt(JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f * JPH::JPH_PI));
            }
            add(capsule->material, shape, capsule->center, rotation);
        }
        if (const MeshCollider* mesh = entity.tryGet<MeshCollider>()) {
            if (const asset::ModelData* data = meshOf(entity)) {
                bool convex = mesh->convex;
                if (!convex && type == BodyType::Dynamic) {
                    warnOnce(entity.handle(), name,
                             "un Mesh Collider no convexo no puede ser dinamico: se usa su envolvente convexa");
                    convex = true;
                }
                if (!convex && mesh->material.is_trigger && type != BodyType::Static) {
                    convex = true;
                }
                add(mesh->material, meshShape(*data, convex, materialFor(mesh->material), name), Vec3{}, Quat{});
                mesh_used = data;
            }
        }
        if (const terrain::Terrain* t = entity.tryGet<terrain::Terrain>(); t != nullptr && t->collision && terrain_provider) {
            if (type == BodyType::Dynamic) {
                warnOnce(entity.handle(), name, "un terreno no puede ser dinamico (se ignora su colision)");
            } else if (const std::shared_ptr<const terrain::TerrainData> data = terrain_provider(entity);
                       data && data->resolution() >= 3) {
                // Jolt quiere 2^n muestras: se remuestrea (res - 1) cubriendo todo el terreno.
                const std::uint32_t samples = std::max<std::uint32_t>(data->resolution() - 1, 4);
                std::vector<float> heights(static_cast<std::size_t>(samples) * samples);
                for (std::uint32_t y = 0; y < samples; ++y) {
                    for (std::uint32_t x = 0; x < samples; ++x) {
                        heights[static_cast<std::size_t>(y) * samples + x] =
                            data->sample(static_cast<float>(x) / static_cast<float>(samples - 1),
                                         static_cast<float>(y) / static_cast<float>(samples - 1)) * t->height;
                    }
                }
                const float cell = t->size / static_cast<float>(samples - 1);
                ColliderMaterial material;
                material.friction = t->friction;
                JPH::HeightFieldShapeSettings settings(heights.data(), JPH::Vec3::sZero(), JPH::Vec3(cell, 1.0f, cell), samples);
                settings.mMaterials.push_back(materialFor(material));
                const auto result = settings.Create();
                if (result.HasError()) {
                    std::cerr << "[Fisica] " << name << ": terreno no valido (" << result.GetError().c_str() << ")\n";
                } else {
                    solid.push_back(ShapePart{result.Get(), Vec3{}, Quat{}});
                }
            }
        }
        if (const PlaneCollider* plane = entity.tryGet<PlaneCollider>()) {
            if (type == BodyType::Dynamic) {
                warnOnce(entity.handle(), name, "un Plane Collider no puede ser dinamico (se ignora)");
            } else {
                add(plane->material,
                    JPH::PlaneShapeSettings(JPH::Plane(JPH::Vec3::sAxisY(), 0.0f), materialFor(plane->material), 1000.0f)
                        .Create()
                        .Get(),
                    Vec3{}, Quat{});
            }
        }
    }

    JPH::RefConst<JPH::Shape> combine(const std::vector<ShapePart>& parts, const Vec3& scale, const std::string& name) {
        if (parts.empty()) return nullptr;
        const auto is_identity = [](const ShapePart& p) {
            return nearlyEqual(p.position, Vec3{}, 1e-6f) && std::abs(p.rotation.w) > 1.0f - 1e-6f;
        };
        JPH::RefConst<JPH::Shape> shape;
        if (parts.size() == 1 && is_identity(parts[0])) {
            shape = parts[0].shape;
        } else if (parts.size() == 1) {
            const auto result =
                JPH::RotatedTranslatedShapeSettings(toJolt(parts[0].position), toJolt(parts[0].rotation), parts[0].shape)
                    .Create();
            if (result.HasError()) {
                std::cerr << "[Fisica] " << name << ": " << result.GetError().c_str() << "\n";
                return nullptr;
            }
            shape = result.Get();
        } else {
            JPH::StaticCompoundShapeSettings compound;
            for (const ShapePart& p : parts) {
                compound.AddShape(toJolt(p.position), toJolt(p.rotation), p.shape);
            }
            const auto result = compound.Create();
            if (result.HasError()) {
                std::cerr << "[Fisica] " << name << ": " << result.GetError().c_str() << "\n";
                return nullptr;
            }
            shape = result.Get();
        }
        if (!sameScale(scale, Vec3{1.0f, 1.0f, 1.0f})) {
            // Jolt ajusta las escalas que una forma no admite (p. ej. una
            // esfera con escala no uniforme usa la media).
            const auto scaled = shape->ScaleShape(toJolt(scale));
            if (scaled.HasError()) {
                std::cerr << "[Fisica] " << name << ": escala no valida (" << scaled.GetError().c_str() << ")\n";
            } else {
                shape = scaled.Get();
            }
        }
        return shape;
    }

    // --- Sincronizacion con el World ---

    // Firma de todo lo que da forma al cuerpo: un hash (FNV-1a) de los
    // valores de sus componentes. Se calcula cada frame por entidad: sin
    // reservas de memoria, solo sumar numeros.
    struct Signature {
        std::uint64_t hash = 1469598103934665603ull;
        void push_back(float value) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            push_bits(bits);
        }
        void push_bits(std::uint32_t bits) { hash = (hash ^ bits) * 1099511628211ull; }
    };
    // Los pools de EnTT de lo que mira la sincronizacion, tomados una vez:
    // registry.try_get busca el pool en un mapa en cada llamada y, con
    // cientos de objetos por frame, eso era lo que mas costaba.
    struct Pools {
        explicit Pools(entt::registry& r)
            : info(r.storage<ecs::EntityInfo>()),
              hierarchy(r.storage<ecs::Hierarchy>()),
              rigidbody(r.storage<Rigidbody>()),
              box(r.storage<BoxCollider>()),
              sphere(r.storage<SphereCollider>()),
              capsule(r.storage<CapsuleCollider>()),
              mesh(r.storage<MeshCollider>()),
              plane(r.storage<PlaneCollider>()),
              terrain(r.storage<terrain::Terrain>()) {}
        template <typename Storage>
        static auto* find(Storage& storage, entt::entity e) {
            return storage.contains(e) ? &storage.get(e) : nullptr;
        }
        // activeInHierarchy sin pasar por el registry: la entidad y sus padres.
        bool active(entt::entity e) const {
            while (e != entt::null) {
                if (info.contains(e) && !info.get(e).active) return false;
                e = hierarchy.contains(e) ? hierarchy.get(e).parent : entt::null;
            }
            return true;
        }
        entt::storage_for_t<ecs::EntityInfo>& info;
        entt::storage_for_t<ecs::Hierarchy>& hierarchy;
        entt::storage_for_t<Rigidbody>& rigidbody;
        entt::storage_for_t<BoxCollider>& box;
        entt::storage_for_t<SphereCollider>& sphere;
        entt::storage_for_t<CapsuleCollider>& capsule;
        entt::storage_for_t<MeshCollider>& mesh;
        entt::storage_for_t<PlaneCollider>& plane;
        entt::storage_for_t<terrain::Terrain>& terrain;
    };

    static std::uint64_t signatureOf(Pools& pools, entt::entity entity, int layer) {
        Signature s;
        const auto push_material = [&](const ColliderMaterial& m) {
            s.push_back(m.is_trigger ? 1.0f : 0.0f);
            s.push_back(m.friction);
            s.push_back(m.bounciness);
            s.push_bits(m.include_layers);
            s.push_bits(m.exclude_layers);
        };
        const auto push3 = [&](const Vec3& v) {
            s.push_back(v.x);
            s.push_back(v.y);
            s.push_back(v.z);
        };
        s.push_back(static_cast<float>(layer));
        if (const Rigidbody* rb = Pools::find(pools.rigidbody, entity)) {
            s.push_back(1.0f + static_cast<float>(rb->type));
            s.push_back(rb->mass);
            s.push_back(rb->linear_damping);
            s.push_back(rb->angular_damping);
            s.push_back(rb->use_gravity ? rb->gravity_scale : 0.0f);
            s.push_back(static_cast<float>(rb->lock_position_x | rb->lock_position_y << 1 | rb->lock_position_z << 2 |
                                           rb->lock_rotation_x << 3 | rb->lock_rotation_y << 4 | rb->lock_rotation_z << 5 |
                                           rb->continuous << 6 | rb->allow_sleep << 7));
            push3(rb->initial_velocity);
            push3(rb->initial_angular_velocity);
            s.push_back(rb->interpolate ? 1.0f : 0.0f);
            s.push_bits(rb->include_layers);
            s.push_bits(rb->exclude_layers);
        } else {
            s.push_back(0.0f);
        }
        if (const BoxCollider* c = Pools::find(pools.box, entity)) {
            s.push_back(10.0f);
            push_material(c->material);
            push3(c->size);
            push3(c->center);
        }
        if (const SphereCollider* c = Pools::find(pools.sphere, entity)) {
            s.push_back(11.0f);
            push_material(c->material);
            s.push_back(c->radius);
            push3(c->center);
        }
        if (const CapsuleCollider* c = Pools::find(pools.capsule, entity)) {
            s.push_back(12.0f);
            push_material(c->material);
            s.push_back(c->radius);
            s.push_back(c->height);
            s.push_back(static_cast<float>(c->axis));
            push3(c->center);
        }
        if (const MeshCollider* c = Pools::find(pools.mesh, entity)) {
            s.push_back(13.0f);
            push_material(c->material);
            s.push_back(c->convex ? 1.0f : 0.0f);
        }
        if (const PlaneCollider* c = Pools::find(pools.plane, entity)) {
            s.push_back(14.0f);
            push_material(c->material);
        }
        if (const terrain::Terrain* t = Pools::find(pools.terrain, entity); t != nullptr && t->collision) {
            s.push_back(15.0f);
            s.push_back(t->size);
            s.push_back(t->height);
            s.push_back(t->friction);
        }
        return s.hash;
    }

    void destroyEntry(BodyEntry& entry) {
        for (JPH::BodyID* id : {&entry.solid, &entry.sensor}) {
            if (id->IsInvalid()) continue;
            const std::uint32_t key = id->GetIndexAndSequenceNumber();
            body_entities.erase(key);
            if (layer_overrides.erase(key) != 0) overrides_dirty = true;
            // Las parejas de este cuerpo terminan (Exit).
            for (auto it = pairs.begin(); it != pairs.end();) {
                if (it->second.body_a == key || it->second.body_b == key) {
                    emitExit(it->second);
                    it = pairs.erase(it);
                } else {
                    ++it;
                }
            }
            bodies().RemoveBody(*id);
            bodies().DestroyBody(*id);
            *id = JPH::BodyID();
        }
    }

    JPH::BodyID createBody(const JPH::Shape* shape, const Vec3& position, const Quat& rotation, JPH::EMotionType motion,
                           JPH::ObjectLayer layer, entt::entity entity, bool sensor,
                           const std::function<void(JPH::BodyCreationSettings&)>& configure) {
        JPH::BodyCreationSettings settings(shape, JPH::RVec3(toJolt(position)), toJolt(rotation), motion, layer);
        settings.mUserData = entityToUserData(entity);
        settings.mIsSensor = sensor;
        if (motion == JPH::EMotionType::Kinematic) {
            settings.mCollideKinematicVsNonDynamic = true;
        }
        if (configure) configure(settings);
        JPH::Body* body = bodies().CreateBody(settings);
        if (body == nullptr) {
            std::cerr << "[Fisica] No quedan cuerpos libres (sube max_bodies en los ajustes de fisica)\n";
            return JPH::BodyID();
        }
        const JPH::BodyID id = body->GetID();
        const bool activate = motion != JPH::EMotionType::Static;
        bodies().AddBody(id, activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        body_entities[id.GetIndexAndSequenceNumber()] = entity;
        return id;
    }

    void buildEntry(ecs::Entity entity, BodyEntry& entry, bool simulate) {
        const std::string& name = entity.name();
        const Rigidbody* rb = entity.tryGet<Rigidbody>();
        entry.type = rb != nullptr ? rb->type : BodyType::Static;
        entry.interpolate = rb == nullptr || rb->interpolate;
        entry.previous_position = entry.current_position = entry.position;
        entry.previous_rotation = entry.current_rotation = entry.rotation;

        std::vector<ShapePart> solid_parts;
        std::vector<ShapePart> sensor_parts;
        entry.mesh = nullptr;
        collectShapes(entity, entry.type, solid_parts, sensor_parts, entry.mesh);
        const JPH::RefConst<JPH::Shape> solid = combine(solid_parts, entry.scale, name);
        const JPH::RefConst<JPH::Shape> sensor = combine(sensor_parts, entry.scale, name);

        const int layer = entity.tryGet<ecs::EntityInfo>() != nullptr ? entity.get<ecs::EntityInfo>().layer : 0;
        const bool moving = entry.type != BodyType::Static;
        entry.layer = std::clamp(layer, 0, kLayerCount - 1);
        // Capas anuladas: las del Rigidbody y las de los colliders de cada cuerpo.
        LayerOverride solid_override{rb != nullptr ? rb->include_layers : 0u, rb != nullptr ? rb->exclude_layers : 0u};
        LayerOverride sensor_override = solid_override;
        const auto add_override = [&](const ColliderMaterial& m) {
            LayerOverride& o = m.is_trigger ? sensor_override : solid_override;
            o.include |= m.include_layers;
            o.exclude |= m.exclude_layers;
        };
        if (const auto* c = entity.tryGet<BoxCollider>()) add_override(c->material);
        if (const auto* c = entity.tryGet<SphereCollider>()) add_override(c->material);
        if (const auto* c = entity.tryGet<CapsuleCollider>()) add_override(c->material);
        if (const auto* c = entity.tryGet<MeshCollider>()) add_override(c->material);
        if (const auto* c = entity.tryGet<PlaneCollider>()) add_override(c->material);

        if (solid != nullptr) {
            JPH::EMotionType motion = entry.type == BodyType::Dynamic   ? JPH::EMotionType::Dynamic
                                      : entry.type == BodyType::Kinematic ? JPH::EMotionType::Kinematic
                                                                          : JPH::EMotionType::Static;
            // Todo congelado = no se mueve: cinematico.
            if (rb != nullptr && motion == JPH::EMotionType::Dynamic && rb->lock_position_x && rb->lock_position_y &&
                rb->lock_position_z && rb->lock_rotation_x && rb->lock_rotation_y && rb->lock_rotation_z) {
                motion = JPH::EMotionType::Kinematic;
            }
            entry.solid = createBody(solid, entry.position, entry.rotation, motion, objectLayer(layer, moving),
                                     entity.handle(), false, [&](JPH::BodyCreationSettings& s) {
                // La friccion y el rebote de verdad los pone el listener por
                // collider (ColliderPhysicsMaterial).
                s.mFriction = 0.6f;
                if (rb == nullptr || motion != JPH::EMotionType::Dynamic) return;
                s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                s.mMassPropertiesOverride.mMass = std::max(rb->mass, 1e-3f);
                s.mLinearDamping = std::max(rb->linear_damping, 0.0f);
                s.mAngularDamping = std::max(rb->angular_damping, 0.0f);
                s.mGravityFactor = rb->use_gravity ? rb->gravity_scale : 0.0f;
                s.mMotionQuality = rb->continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
                s.mAllowSleeping = rb->allow_sleep;
                JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;
                if (rb->lock_position_x) dofs &= ~JPH::EAllowedDOFs::TranslationX;
                if (rb->lock_position_y) dofs &= ~JPH::EAllowedDOFs::TranslationY;
                if (rb->lock_position_z) dofs &= ~JPH::EAllowedDOFs::TranslationZ;
                if (rb->lock_rotation_x) dofs &= ~JPH::EAllowedDOFs::RotationX;
                if (rb->lock_rotation_y) dofs &= ~JPH::EAllowedDOFs::RotationY;
                if (rb->lock_rotation_z) dofs &= ~JPH::EAllowedDOFs::RotationZ;
                s.mAllowedDOFs = dofs;
                if (simulate) {
                    s.mLinearVelocity = toJolt(rb->initial_velocity);
                    s.mAngularVelocity = toJolt(rb->initial_angular_velocity);
                }
            });
        }
        if (!entry.solid.IsInvalid() && (solid_override.include | solid_override.exclude) != 0) {
            layer_overrides[entry.solid.GetIndexAndSequenceNumber()] = solid_override;
            overrides_dirty = true;
        }
        if (sensor != nullptr) {
            // El trigger de algo que se mueve es cinematico y lo acompana;
            // despierto siempre (dormido no detectaria nada).
            entry.sensor = createBody(sensor, entry.position, entry.rotation,
                                      moving ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static,
                                      objectLayer(layer, moving), entity.handle(), true,
                                      [&](JPH::BodyCreationSettings& s) { s.mAllowSleeping = !moving; });
            if (!entry.sensor.IsInvalid() && (sensor_override.include | sensor_override.exclude) != 0) {
                layer_overrides[entry.sensor.GetIndexAndSequenceNumber()] = sensor_override;
                overrides_dirty = true;
            }
        }
    }

    void sync(ecs::World& w, bool simulate) {
        world = &w;
        entt::registry& registry = w.registry();
        std::vector<entt::entity>& candidates = sync_candidates;  // se reutiliza
        candidates.clear();
        const auto gather = [&](auto view) {
            for (const entt::entity e : view) candidates.push_back(e);
        };
        gather(registry.view<BoxCollider>());
        gather(registry.view<SphereCollider>());
        gather(registry.view<CapsuleCollider>());
        gather(registry.view<MeshCollider>());
        gather(registry.view<PlaneCollider>());
        gather(registry.view<terrain::Terrain>());
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        ++sync_generation;
        Pools pools(registry);
        for (const entt::entity handle : candidates) {
            const ecs::Entity entity = w.wrap(handle);
            if (!pools.active(handle)) continue;
            const core::Mat4& world_matrix = entity.worldMatrix();
            const ecs::EntityInfo* info = Pools::find(pools.info, handle);
            const std::uint64_t signature = signatureOf(pools, handle, info != nullptr ? info->layer : 0);
            auto it = entries.find(handle);
            if (it != entries.end()) it->second.seen = sync_generation;
            const bool has_mesh = pools.mesh.contains(handle);
            // Terreno: sus datos (y su version de colision) tambien cuentan.
            const terrain::Terrain* terrain_comp = Pools::find(pools.terrain, handle);
            const bool is_terrain = terrain_comp != nullptr;
            if (is_terrain && !terrain_comp->collision && !has_mesh && !pools.box.contains(handle) &&
                !pools.sphere.contains(handle) && !pools.capsule.contains(handle) && !pools.plane.contains(handle)) {
                if (it != entries.end()) it->second.seen = 0;  // si tenia cuerpo, se borra
                continue;  // terreno sin colision y sin otros colliders: sin cuerpo
            }
            std::shared_ptr<const terrain::TerrainData> terrain_data =
                is_terrain && terrain_provider ? terrain_provider(entity) : nullptr;
            const std::uint64_t terrain_key =
                terrain_data ? (terrain_data->collisionVersion() * 1000003ull ^
                                reinterpret_cast<std::uintptr_t>(terrain_data.get()))
                             : 0ull;
            const void* mesh_now = it != entries.end() && has_mesh ? meshOf(entity) : nullptr;
            const bool same_shape = it != entries.end() && it->second.signature == signature &&
                                    (!has_mesh || mesh_now == it->second.mesh) &&
                                    it->second.terrain_key == terrain_key;
            // Lo normal: nada cambio (ni forma ni matriz). Sin descomponer nada.
            if (same_shape && std::memcmp(&it->second.matrix, &world_matrix, sizeof(core::Mat4)) == 0) continue;

            Vec3 position{};
            Quat rotation{};
            Vec3 scale{};
            ecs::decomposeMatrix(world_matrix, position, rotation, scale);
            rotation = core::normalize(rotation);
            if (is_terrain) {
                // El terreno no gira ni escala: solo su esquina cuenta.
                rotation = Quat{};
                scale = Vec3{1.0f, 1.0f, 1.0f};
            }
            const bool rebuild = !same_shape || !sameScale(it->second.scale, scale);
            if (rebuild) {
                if (it != entries.end()) {
                    destroyEntry(it->second);
                } else {
                    it = entries.emplace(handle, BodyEntry{}).first;
                }
                BodyEntry& entry = it->second;
                entry.seen = sync_generation;
                entry.entity = handle;
                entry.signature = signature;
                entry.terrain_key = terrain_key;
                entry.matrix = world_matrix;
                entry.position = position;
                entry.rotation = rotation;
                entry.scale = scale;
                buildEntry(entity, entry, simulate);
                continue;
            }

            // Mismo cuerpo: si el Transform se movio, se recoloca.
            BodyEntry& entry = it->second;
            entry.matrix = world_matrix;
            if (nearlyEqual(entry.position, position, 1e-5f) && sameRotation(entry.rotation, rotation)) continue;
            entry.position = position;
            entry.rotation = rotation;
            // Teletransporte: sin interpolar desde donde estaba.
            entry.previous_position = entry.current_position = position;
            entry.previous_rotation = entry.current_rotation = rotation;
            const bool kinematic_follow = simulate && entry.type == BodyType::Kinematic;
            if (kinematic_follow) continue;  // se mueve en el paso (MoveKinematic)
            const JPH::EActivation activation =
                entry.type == BodyType::Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
            for (const JPH::BodyID& id : {entry.solid, entry.sensor}) {
                if (!id.IsInvalid()) {
                    bodies().SetPositionAndRotation(id, JPH::RVec3(toJolt(position)), toJolt(rotation), activation);
                }
            }
        }

        // Borradas, desactivadas o sin colliders.
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->second.seen != sync_generation) {
                destroyEntry(it->second);
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
        if (overrides_dirty) rebuildLayerIncludes();
    }

    // Antes de cada paso: los cinematicos van hacia su Transform y los
    // sensores de lo dinamico, hacia su cuerpo.
    void moveKinematics(float dt) {
        for (auto& [handle, entry] : entries) {
            if (entry.type == BodyType::Kinematic) {
                for (const JPH::BodyID& id : {entry.solid, entry.sensor}) {
                    if (!id.IsInvalid()) {
                        bodies().MoveKinematic(id, JPH::RVec3(toJolt(entry.position)), toJolt(entry.rotation), dt);
                    }
                }
            } else if (entry.type == BodyType::Dynamic && !entry.sensor.IsInvalid()) {
                JPH::RVec3 position = JPH::RVec3(toJolt(entry.position));
                JPH::Quat rotation = toJolt(entry.rotation);
                if (!entry.solid.IsInvalid()) {
                    bodies().GetPositionAndRotation(entry.solid, position, rotation);
                }
                bodies().MoveKinematic(entry.sensor, position, rotation, dt);
            }
        }
    }

    // Tras cada paso: la pose fisica de los dinamicos (la anterior se guarda
    // para interpolar).
    void capturePoses() {
        for (auto& [handle, entry] : entries) {
            if (entry.type != BodyType::Dynamic || entry.solid.IsInvalid()) continue;
            entry.previous_position = entry.current_position;
            entry.previous_rotation = entry.current_rotation;
            JPH::RVec3 p;
            JPH::Quat q;
            bodies().GetPositionAndRotation(entry.solid, p, q);
            entry.current_position = fromJolt(JPH::Vec3(p));
            entry.current_rotation = fromJolt(q.Normalized());
        }
    }

    // Los dinamicos escriben su Transform: entre la pose anterior y la
    // actual segun lo que falta para el siguiente paso (`alpha`, 0..1). Asi
    // se mueven suaves aunque se dibuje a mas FPS que la fisica.
    void writeBack(ecs::World& w, float alpha) {
        for (auto& [handle, entry] : entries) {
            if (entry.type != BodyType::Dynamic || entry.solid.IsInvalid() || !w.valid(handle)) continue;
            const float t = entry.interpolate ? std::clamp(alpha, 0.0f, 1.0f) : 1.0f;
            const Vec3 position = core::lerp(entry.previous_position, entry.current_position, t);
            const Quat rotation = core::slerp(entry.previous_rotation, entry.current_rotation, t);
            if (nearlyEqual(entry.position, position, 1e-6f) && sameRotation(entry.rotation, rotation)) continue;
            entry.position = position;
            entry.rotation = rotation;
            ecs::Entity entity = w.wrap(handle);
            entity.setWorldMatrix(core::composeTrs(position, rotation, entry.scale));
            entry.matrix = entity.worldMatrix();
        }
    }

    // --- Contactos -> eventos ---

    PhysicsEvent eventFrom(const Pair& pair, PhysicsEventType type) const {
        PhysicsEvent e;
        e.type = type;
        e.a = world->wrap(pair.a);
        e.b = world->wrap(pair.b);
        e.point = pair.last.point;
        e.normal = pair.swapped ? -pair.last.normal : pair.last.normal;
        e.relative_velocity = pair.swapped ? -pair.last.relative_velocity : pair.last.relative_velocity;
        e.penetration = pair.last.penetration;
        e.contact_count = pair.last.count;
        e.step = steps;
        return e;
    }

    void emitExit(const Pair& pair) {
        if (world == nullptr) return;
        dispatch(eventFrom(pair, pair.trigger ? PhysicsEventType::TriggerExit : PhysicsEventType::CollisionExit));
    }

    // Cuerpo que existe, se puede mover y esta dormido.
    bool sleeping(std::uint32_t body) const {
        if (body_entities.count(body) == 0) return false;
        const JPH::BodyID id(body);
        JPH::BodyLockRead lock(system->GetBodyLockInterface(), id);
        return lock.Succeeded() && !lock.GetBody().IsStatic() && !lock.GetBody().IsActive();
    }

    void processContacts() {
        std::vector<RawContact> raw;
        {
            std::lock_guard lock(contacts_mutex);
            raw.swap(raw_contacts);
        }
        contact_points.clear();
        for (auto& [key, pair] : pairs) {
            pair.touched = false;
            pair.entered = false;
        }
        std::vector<std::uint64_t> order;  // Enter en el orden en que llegaron
        for (const RawContact& c : raw) {
            const std::uint64_t key = pairKey(c.body1, c.body2);
            if (c.kind == RawContact::Kind::Removed) {
                const auto it = pairs.find(key);
                if (it == pairs.end()) continue;
                // Jolt quita los contactos de un cuerpo que se duerme; Unity no
                // da Exit por dormirse (solo deja de dar Stay): la pareja se
                // conserva hasta que se separen de verdad o se borre.
                if (sleeping(c.body1) || sleeping(c.body2)) continue;
                it->second.subs.erase(c.sub_key);
                continue;  // el Exit se decide al final (puede volver a tocarse en el mismo paso)
            }
            if (contact_points.size() < 4096) {
                contact_points.push_back(ContactDebug{c.point, c.normal, c.sensor1 || c.sensor2});
            }
            auto it = pairs.find(key);
            if (it == pairs.end()) {
                // Solo cuerpos que siguen siendo nuestros.
                if (body_entities.count(c.body1) == 0 || body_entities.count(c.body2) == 0) continue;
                Pair pair;
                pair.trigger = c.sensor1 || c.sensor2;
                // En los triggers, `a` es el sensor.
                pair.swapped = pair.trigger && !c.sensor1;
                pair.a = pair.swapped ? c.entity2 : c.entity1;
                pair.b = pair.swapped ? c.entity1 : c.entity2;
                pair.body_a = pair.swapped ? c.body2 : c.body1;
                pair.body_b = pair.swapped ? c.body1 : c.body2;
                it = pairs.emplace(key, std::move(pair)).first;
                it->second.entered = true;
                order.push_back(key);
            }
            Pair& pair = it->second;
            // Normal de body1 a body2 tal como llega; eventFrom la orienta.
            RawContact oriented = c;
            if (c.body1 != (pair.swapped ? pair.body_b : pair.body_a)) {
                oriented.normal = -c.normal;
                oriented.relative_velocity = -c.relative_velocity;
            }
            pair.last = oriented;
            pair.subs.insert(c.sub_key);
            pair.touched = true;
        }

        for (const std::uint64_t key : order) {
            const auto it = pairs.find(key);
            if (it == pairs.end()) continue;
            dispatch(eventFrom(it->second,
                               it->second.trigger ? PhysicsEventType::TriggerEnter : PhysicsEventType::CollisionEnter));
        }
        for (auto it = pairs.begin(); it != pairs.end();) {
            Pair& pair = it->second;
            if (pair.subs.empty()) {
                emitExit(pair);
                it = pairs.erase(it);
                continue;
            }
            if (pair.touched && !pair.entered) {
                dispatch(eventFrom(pair, pair.trigger ? PhysicsEventType::TriggerStay : PhysicsEventType::CollisionStay));
            }
            ++it;
        }
    }

    void stepOnce(ecs::World& w) {
        const float dt = settings.fixed_step;
        moveKinematics(dt);
        system->Update(dt, std::max(settings.collision_steps, 1), temp_allocator.get(), job_system.get());
        ++steps;
        capturePoses();
        processContacts();
        (void)w;
    }

    // --- Consultas ---

    void recordQuery(const QueryDebug& q) const {
        if (!record_queries) return;
        if (queries.size() >= 256) queries.erase(queries.begin());
        queries.push_back(q);
    }

    void ignoreIds(const QueryFilter& filter, JPH::BodyID& a, JPH::BodyID& b) const {
        if (const BodyEntry* entry = entryOf(filter.ignore)) {
            a = entry->solid;
            b = entry->sensor;
        }
    }

    RaycastHit hitFrom(const JPH::BodyID& id, const JPH::SubShapeID& sub, const Vec3& origin, const Vec3& direction,
                       float distance) const {
        RaycastHit hit;
        hit.entity = entityOfBody(id);
        hit.distance = distance;
        hit.point = origin + direction * distance;
        hit.normal = -direction;
        JPH::BodyLockRead lock(system->GetBodyLockInterface(), id);
        if (lock.Succeeded()) {
            const JPH::Body& body = lock.GetBody();
            if (distance > 0.0f) {
                hit.normal = fromJolt(body.GetWorldSpaceSurfaceNormal(sub, JPH::RVec3(toJolt(hit.point))));
            }
            hit.trigger = body.IsSensor();
            hit.layer = body.GetObjectLayer() & 31;
        }
        return hit;
    }
};

// -----------------------------------------------------------------------------
// PhysicsSystem
// -----------------------------------------------------------------------------

PhysicsSystem::PhysicsSystem() : impl_(std::make_unique<Impl>()) {
    ensureJolt();
    registerPhysicsComponents();
}

PhysicsSystem::~PhysicsSystem() {
    stop();
}

void PhysicsSystem::setSettings(const PhysicsSettings& settings) {
    impl_->settings = settings;
    impl_->applySettings();
}

const PhysicsSettings& PhysicsSystem::settings() const {
    return impl_->settings;
}

void PhysicsSystem::setMeshProvider(MeshProvider provider) {
    impl_->mesh_provider = std::move(provider);
}

void PhysicsSystem::setTerrainProvider(TerrainProvider provider) {
    impl_->terrain_provider = std::move(provider);
}

void PhysicsSystem::setAssetManager(assets::AssetManager* manager) {
    impl_->asset_manager = manager;
}

void PhysicsSystem::start(ecs::World& world) {
    stop();
    Impl& d = *impl_;
    d.world = &world;
    d.temp_allocator = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(32 * 1024 * 1024);
    const unsigned hardware = std::max(2u, std::thread::hardware_concurrency());
    const int threads = d.settings.worker_threads > 0 ? static_cast<int>(d.settings.worker_threads)
                                                      : static_cast<int>(std::min(hardware - 1, 8u));
    d.job_system = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
    d.system = std::make_unique<JPH::PhysicsSystem>();
    const JPH::uint max_bodies = std::max<JPH::uint>(d.settings.max_bodies, 16);
    d.system->Init(max_bodies, 0, max_bodies, max_bodies, d.broadphase_layers, d.object_vs_broadphase,
                   d.layer_filter);
    d.system->SetContactListener(&d.listener);
    d.applySettings();
    d.accumulator = 0.0f;
    d.steps = 0;
    d.events.clear();
    d.contact_points.clear();
    d.pairs.clear();
    d.warned.clear();
}

void PhysicsSystem::stop() {
    Impl& d = *impl_;
    if (!d.system) return;
    for (auto& [handle, entry] : d.entries) {
        for (const JPH::BodyID& id : {entry.solid, entry.sensor}) {
            if (id.IsInvalid()) continue;
            d.bodies().RemoveBody(id);
            d.bodies().DestroyBody(id);
        }
    }
    d.entries.clear();
    d.body_entities.clear();
    d.layer_overrides.clear();
    d.layer_includes.fill(0);
    d.pairs.clear();
    {
        std::lock_guard lock(d.contacts_mutex);
        d.raw_contacts.clear();
    }
    d.system.reset();
    d.job_system.reset();
    d.temp_allocator.reset();
    d.mesh_shapes.clear();
    d.contact_points.clear();
    d.queries.clear();
    d.world = nullptr;
}

bool PhysicsSystem::running() const {
    return impl_->system != nullptr;
}

int PhysicsSystem::update(ecs::World& world, float delta_seconds, bool simulate) {
    Impl& d = *impl_;
    if (!d.system) return 0;
    d.events.clear();
    const auto begin = std::chrono::steady_clock::now();
    d.sync(world, simulate);
    int steps = 0;
    if (simulate) {
        const float step = d.settings.fixed_step;
        d.accumulator += std::clamp(delta_seconds, 0.0f, 0.25f);
        while (d.accumulator >= step && steps < d.settings.max_substeps) {
            d.stepOnce(world);
            d.accumulator -= step;
            ++steps;
        }
        // Si el frame fue demasiado largo, la simulacion se ralentiza en vez
        // de acumular retraso.
        d.accumulator = std::min(d.accumulator, step);
        d.writeBack(world, d.accumulator / step);
    } else {
        d.accumulator = 0.0f;
        d.contact_points.clear();
    }
    d.step_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - begin).count();
    return steps;
}

void PhysicsSystem::singleStep(ecs::World& world) {
    Impl& d = *impl_;
    if (!d.system) return;
    d.events.clear();
    d.sync(world, true);
    d.stepOnce(world);
    d.writeBack(world, 1.0f);
}

// --- Consultas ---------------------------------------------------------------

bool PhysicsSystem::raycast(const Vec3& origin, const Vec3& direction, float max_distance, RaycastHit& hit,
                            const QueryFilter& filter) const {
    const Impl& d = *impl_;
    QueryDebug debug;
    debug.kind = QueryDebug::Kind::Ray;
    debug.origin = origin;
    const float length = core::length(direction);
    if (!d.system || length < 1e-8f || max_distance <= 0.0f) return false;
    const Vec3 dir = direction * (1.0f / length);
    debug.end = origin + dir * max_distance;

    JPH::BodyID ignore_a, ignore_b;
    d.ignoreIds(filter, ignore_a, ignore_b);
    const MaskLayerFilter layers(filter.layer_mask);
    const QueryBodyFilter body_filter(d.hitTriggers(filter), ignore_a, ignore_b);
    JPH::RRayCast ray(JPH::RVec3(toJolt(origin)), toJolt(dir * max_distance));
    JPH::RayCastSettings settings;
    settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
    settings.mTreatConvexAsSolid = true;
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    d.system->GetNarrowPhaseQuery().CastRay(ray, settings, collector, {}, layers, body_filter);
    if (!collector.HadHit()) {
        if (filter.record) d.recordQuery(debug);
        return false;
    }
    hit = d.hitFrom(collector.mHit.mBodyID, collector.mHit.mSubShapeID2, origin, dir,
                    collector.mHit.mFraction * max_distance);
    debug.hit = true;
    debug.hit_point = hit.point;
    debug.hit_normal = hit.normal;
    debug.end = hit.point;
    debug.hits = 1;
    if (filter.record) d.recordQuery(debug);
    return true;
}

std::vector<RaycastHit> PhysicsSystem::raycastAll(const Vec3& origin, const Vec3& direction, float max_distance,
                                                  const QueryFilter& filter) const {
    const Impl& d = *impl_;
    std::vector<RaycastHit> result;
    const float length = core::length(direction);
    if (!d.system || length < 1e-8f || max_distance <= 0.0f) return result;
    const Vec3 dir = direction * (1.0f / length);

    JPH::BodyID ignore_a, ignore_b;
    d.ignoreIds(filter, ignore_a, ignore_b);
    const MaskLayerFilter layers(filter.layer_mask);
    const QueryBodyFilter body_filter(d.hitTriggers(filter), ignore_a, ignore_b);
    JPH::RRayCast ray(JPH::RVec3(toJolt(origin)), toJolt(dir * max_distance));
    JPH::RayCastSettings settings;
    settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
    settings.mTreatConvexAsSolid = true;
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    d.system->GetNarrowPhaseQuery().CastRay(ray, settings, collector, {}, layers, body_filter);
    collector.Sort();
    std::unordered_set<std::uint32_t> seen;  // un impacto por cuerpo (el mas cercano)
    for (const JPH::RayCastResult& r : collector.mHits) {
        if (!seen.insert(r.mBodyID.GetIndexAndSequenceNumber()).second) continue;
        result.push_back(d.hitFrom(r.mBodyID, r.mSubShapeID2, origin, dir, r.mFraction * max_distance));
    }
    QueryDebug debug;
    debug.kind = QueryDebug::Kind::Ray;
    debug.origin = origin;
    debug.end = origin + dir * max_distance;
    debug.hit = !result.empty();
    debug.hits = static_cast<int>(result.size());
    if (!result.empty()) {
        debug.hit_point = result.front().point;
        debug.hit_normal = result.front().normal;
    }
    if (filter.record) d.recordQuery(debug);
    return result;
}

bool PhysicsSystem::sphereCast(const Vec3& origin, float radius, const Vec3& direction, float max_distance,
                               RaycastHit& hit, const QueryFilter& filter) const {
    const Impl& d = *impl_;
    const float length = core::length(direction);
    if (!d.system || length < 1e-8f || radius <= 0.0f) return false;
    const Vec3 dir = direction * (1.0f / length);

    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded();
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        &sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JPH::RVec3(toJolt(origin))),
        toJolt(dir * max_distance));
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::BodyID ignore_a, ignore_b;
    d.ignoreIds(filter, ignore_a, ignore_b);
    const MaskLayerFilter layers(filter.layer_mask);
    const QueryBodyFilter body_filter(d.hitTriggers(filter), ignore_a, ignore_b);
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    d.system->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, layers, body_filter);

    QueryDebug debug;
    debug.kind = QueryDebug::Kind::SphereCast;
    debug.origin = origin;
    debug.extents = Vec3{radius, radius, radius};
    debug.end = origin + dir * max_distance;
    if (!collector.HadHit()) {
        if (filter.record) d.recordQuery(debug);
        return false;
    }
    const JPH::ShapeCastResult& r = collector.mHit;
    hit = RaycastHit{};
    hit.entity = d.entityOfBody(r.mBodyID2);
    hit.distance = r.mFraction * max_distance;
    hit.point = fromJolt(JPH::Vec3(r.mContactPointOn2));
    const JPH::Vec3 axis = r.mPenetrationAxis;
    hit.normal = axis.LengthSq() > 1e-12f ? fromJolt(-axis.Normalized()) : -dir;
    {
        JPH::BodyLockRead lock(d.system->GetBodyLockInterface(), r.mBodyID2);
        if (lock.Succeeded()) {
            hit.trigger = lock.GetBody().IsSensor();
            hit.layer = lock.GetBody().GetObjectLayer() & 31;
        }
    }
    debug.hit = true;
    debug.hit_point = hit.point;
    debug.hit_normal = hit.normal;
    debug.end = origin + dir * hit.distance;
    debug.hits = 1;
    if (filter.record) d.recordQuery(debug);
    return true;
}

std::vector<ecs::Entity> PhysicsSystem::overlapSphere(const Vec3& center, float radius,
                                                      const QueryFilter& filter) const {
    const Impl& d = *impl_;
    std::vector<ecs::Entity> result;
    if (!d.system || radius <= 0.0f) return result;
    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded();
    JPH::BodyID ignore_a, ignore_b;
    d.ignoreIds(filter, ignore_a, ignore_b);
    const MaskLayerFilter layers(filter.layer_mask);
    const QueryBodyFilter body_filter(d.hitTriggers(filter), ignore_a, ignore_b);
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    d.system->GetNarrowPhaseQuery().CollideShape(&sphere, JPH::Vec3::sReplicate(1.0f),
                                                 JPH::RMat44::sTranslation(JPH::RVec3(toJolt(center))), settings,
                                                 JPH::RVec3::sZero(), collector, {}, layers, body_filter);
    std::unordered_set<std::uint32_t> seen;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const ecs::Entity e = d.entityOfBody(r.mBodyID2);
        if (e.valid() && seen.insert(static_cast<std::uint32_t>(entt::to_integral(e.handle()))).second) {
            result.push_back(e);
        }
    }
    QueryDebug debug;
    debug.kind = QueryDebug::Kind::OverlapSphere;
    debug.origin = center;
    debug.end = center;
    debug.extents = Vec3{radius, radius, radius};
    debug.hit = !result.empty();
    debug.hits = static_cast<int>(result.size());
    if (filter.record) d.recordQuery(debug);
    return result;
}

std::vector<ecs::Entity> PhysicsSystem::overlapBox(const Vec3& center, const Vec3& half_extents, const Quat& rotation,
                                                   const QueryFilter& filter) const {
    const Impl& d = *impl_;
    std::vector<ecs::Entity> result;
    if (!d.system) return result;
    const Vec3 half{std::max(half_extents.x, 1e-3f), std::max(half_extents.y, 1e-3f), std::max(half_extents.z, 1e-3f)};
    JPH::BoxShape box(toJolt(half), std::min({JPH::cDefaultConvexRadius, half.x * 0.5f, half.y * 0.5f, half.z * 0.5f}));
    box.SetEmbedded();
    JPH::BodyID ignore_a, ignore_b;
    d.ignoreIds(filter, ignore_a, ignore_b);
    const MaskLayerFilter layers(filter.layer_mask);
    const QueryBodyFilter body_filter(d.hitTriggers(filter), ignore_a, ignore_b);
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    d.system->GetNarrowPhaseQuery().CollideShape(
        &box, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sRotationTranslation(toJolt(rotation), JPH::RVec3(toJolt(center))),
        settings, JPH::RVec3::sZero(), collector, {}, layers, body_filter);
    std::unordered_set<std::uint32_t> seen;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const ecs::Entity e = d.entityOfBody(r.mBodyID2);
        if (e.valid() && seen.insert(static_cast<std::uint32_t>(entt::to_integral(e.handle()))).second) {
            result.push_back(e);
        }
    }
    QueryDebug debug;
    debug.kind = QueryDebug::Kind::OverlapBox;
    debug.origin = center;
    debug.end = center;
    debug.extents = half;
    debug.rotation = rotation;
    debug.hit = !result.empty();
    debug.hits = static_cast<int>(result.size());
    if (filter.record) d.recordQuery(debug);
    return result;
}

void PhysicsSystem::setRecordQueries(bool record) {
    impl_->record_queries = record;
    if (!record) impl_->queries.clear();
}

const std::vector<QueryDebug>& PhysicsSystem::recordedQueries() const {
    return impl_->queries;
}

void PhysicsSystem::clearRecordedQueries() {
    impl_->queries.clear();
}

// --- Rigidbody -----------------------------------------------------------------

Vec3 PhysicsSystem::linearVelocity(ecs::Entity entity) const {
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry == nullptr || entry->solid.IsInvalid()) return {};
    return fromJolt(impl_->bodies().GetLinearVelocity(entry->solid));
}

void PhysicsSystem::setLinearVelocity(ecs::Entity entity, const Vec3& velocity) {
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry == nullptr || entry->solid.IsInvalid() || entry->type != BodyType::Dynamic) return;
    impl_->bodies().SetLinearVelocity(entry->solid, toJolt(velocity));
}

Vec3 PhysicsSystem::angularVelocity(ecs::Entity entity) const {
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry == nullptr || entry->solid.IsInvalid()) return {};
    return fromJolt(impl_->bodies().GetAngularVelocity(entry->solid));
}

void PhysicsSystem::setAngularVelocity(ecs::Entity entity, const Vec3& velocity) {
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry == nullptr || entry->solid.IsInvalid() || entry->type != BodyType::Dynamic) return;
    impl_->bodies().SetAngularVelocity(entry->solid, toJolt(velocity));
}

void PhysicsSystem::addForce(ecs::Entity entity, const Vec3& force, ForceMode mode) {
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry == nullptr || entry->solid.IsInvalid() || entry->type != BodyType::Dynamic) return;
    JPH::BodyInterface& bodies = impl_->bodies();
    float mass = 1.0f;
    if (mode == ForceMode::Acceleration || mode == ForceMode::VelocityChange) {
        JPH::BodyLockRead lock(impl_->system->GetBodyLockInterface(), entry->solid);
        if (lock.Succeeded() && lock.GetBody().GetMotionProperties() != nullptr) {
            const float inverse = lock.GetBody().GetMotionProperties()->GetInverseMass();
            mass = inverse > 0.0f ? 1.0f / inverse : 0.0f;
        }
    }
    switch (mode) {
        case ForceMode::Force: bodies.AddForce(entry->solid, toJolt(force)); break;
        case ForceMode::Acceleration: bodies.AddForce(entry->solid, toJolt(force * mass)); break;
        case ForceMode::Impulse: bodies.AddImpulse(entry->solid, toJolt(force)); break;
        case ForceMode::VelocityChange: bodies.AddImpulse(entry->solid, toJolt(force * mass)); break;
    }
}

void PhysicsSystem::addForceAtPosition(ecs::Entity entity, const Vec3& force, const Vec3& position, ForceMode mode) {
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry == nullptr || entry->solid.IsInvalid() || entry->type != BodyType::Dynamic) return;
    JPH::BodyInterface& bodies = impl_->bodies();
    const JPH::RVec3 point(toJolt(position));
    if (mode == ForceMode::Impulse || mode == ForceMode::VelocityChange) {
        bodies.AddImpulse(entry->solid, toJolt(force), point);
    } else {
        bodies.AddForce(entry->solid, toJolt(force), point, JPH::EActivation::Activate);
    }
}

void PhysicsSystem::addTorque(ecs::Entity entity, const Vec3& torque, ForceMode mode) {
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry == nullptr || entry->solid.IsInvalid() || entry->type != BodyType::Dynamic) return;
    JPH::BodyInterface& bodies = impl_->bodies();
    switch (mode) {
        case ForceMode::Force: bodies.AddTorque(entry->solid, toJolt(torque)); break;
        case ForceMode::Impulse: bodies.AddAngularImpulse(entry->solid, toJolt(torque)); break;
        case ForceMode::VelocityChange:
            bodies.SetAngularVelocity(entry->solid, bodies.GetAngularVelocity(entry->solid) + toJolt(torque));
            bodies.ActivateBody(entry->solid);
            break;
        case ForceMode::Acceleration:
            bodies.SetAngularVelocity(entry->solid, bodies.GetAngularVelocity(entry->solid) +
                                                        toJolt(torque * impl_->settings.fixed_step));
            bodies.ActivateBody(entry->solid);
            break;
    }
}

void PhysicsSystem::addImpulse(ecs::Entity entity, const Vec3& impulse) {
    addForce(entity, impulse, ForceMode::Impulse);
}

bool PhysicsSystem::isSleeping(ecs::Entity entity) const {
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry == nullptr || entry->solid.IsInvalid()) return false;
    return !impl_->bodies().IsActive(entry->solid);
}

void PhysicsSystem::wakeUp(ecs::Entity entity) {
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry != nullptr && !entry->solid.IsInvalid()) impl_->bodies().ActivateBody(entry->solid);
}

void PhysicsSystem::sleep(ecs::Entity entity) {
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry != nullptr && !entry->solid.IsInvalid()) impl_->bodies().DeactivateBody(entry->solid);
}

bool PhysicsSystem::hasBody(ecs::Entity entity) const {
    return impl_->entryOf(entity) != nullptr;
}

Vec3 PhysicsSystem::centerOfMass(ecs::Entity entity) const {
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry == nullptr || entry->solid.IsInvalid()) return entity.valid() ? entity.worldPosition() : Vec3{};
    return fromJolt(JPH::Vec3(impl_->bodies().GetCenterOfMassPosition(entry->solid)));
}

// --- Eventos -------------------------------------------------------------------

const std::vector<PhysicsEvent>& PhysicsSystem::events() const {
    return impl_->events;
}

int PhysicsSystem::addListener(EventCallback callback) {
    const int id = impl_->next_listener++;
    impl_->listeners.push_back(
        Impl::ListenerEntry{id, entt::null, std::make_shared<EventCallback>(std::move(callback))});
    return id;
}

int PhysicsSystem::addEntityListener(ecs::Entity entity, EventCallback callback) {
    const int id = impl_->next_listener++;
    impl_->listeners.push_back(
        Impl::ListenerEntry{id, entity.handle(), std::make_shared<EventCallback>(std::move(callback))});
    return id;
}

void PhysicsSystem::removeListener(int id) {
    auto& list = impl_->listeners;
    if (impl_->dispatching > 0) {
        for (Impl::ListenerEntry& l : list) {
            if (l.id == id) l.callback.reset();  // se borra al terminar el evento
        }
        return;
    }
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Impl::ListenerEntry& l) { return l.id == id; }),
               list.end());
}

void PhysicsSystem::reportEvent(const PhysicsEvent& event) {
    PhysicsEvent e = event;
    e.step = impl_->steps;
    impl_->dispatch(e);
}

// --- Depuracion ------------------------------------------------------------------

const std::vector<ContactDebug>& PhysicsSystem::contactPoints() const {
    return impl_->contact_points;
}

bool PhysicsSystem::bodyTriangles(ecs::Entity entity, std::vector<Vec3>& triangles, std::size_t max_triangles) const {
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry == nullptr) return false;
    bool any = false;
    for (const JPH::BodyID& id : {entry->solid, entry->sensor}) {
        if (id.IsInvalid()) continue;
        JPH::BodyLockRead lock(impl_->system->GetBodyLockInterface(), id);
        if (!lock.Succeeded()) continue;
        // Las formas compuestas o escaladas no dan triangulos: se recogen sus
        // hojas (ya con su escala y su posicion en el mundo).
        JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
        lock.GetBody().GetTransformedShape().CollectTransformedShapes(JPH::AABox::sBiggest(), leaves);
        constexpr int kBatch = JPH::Shape::cGetTrianglesMinTrianglesRequested;
        JPH::Float3 vertices[kBatch * 3];
        for (const JPH::TransformedShape& leaf : leaves.mHits) {
            JPH::Shape::GetTrianglesContext context;
            leaf.GetTrianglesStart(context, JPH::AABox::sBiggest(), JPH::RVec3::sZero());
            for (;;) {
                const int count = leaf.GetTrianglesNext(context, kBatch, vertices);
                if (count <= 0) break;
                for (int i = 0; i < count * 3; ++i) {
                    triangles.push_back(Vec3{vertices[i].x, vertices[i].y, vertices[i].z});
                }
                any = true;
                if (triangles.size() / 3 >= max_triangles) return true;
            }
        }
    }
    return any;
}

PhysicsStats PhysicsSystem::stats() const {
    PhysicsStats s;
    const Impl& d = *impl_;
    if (!d.system) return s;
    s.bodies = d.system->GetNumBodies();
    s.active_bodies = d.system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
    for (const auto& [key, pair] : d.pairs) {
        (pair.trigger ? s.trigger_pairs : s.contact_pairs)++;
    }
    s.steps = d.steps;
    s.step_milliseconds = d.step_ms;
    return s;
}

std::uint32_t PhysicsSystem::bodyCount() const {
    return impl_->system ? impl_->system->GetNumBodies() : 0;
}

}  // namespace cramion::physics
