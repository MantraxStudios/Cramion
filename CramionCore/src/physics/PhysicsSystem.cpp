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
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/modeling/EditableMesh.h"
#include "CramionCore/physics/Cloth.h"
#include "CramionCore/physics/Ragdoll.h"
#include "CramionCore/physics/SoftBody.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/water/Water.h"

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
#include <Jolt/Physics/Character/CharacterVirtual.h>
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
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

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
            // El inner body de un personaje: triggers y dinamicos (que no lo
            // atraviesen); con lo quieto ya choca el propio personaje.
            if (!body1.IsSensor() && !body2.IsSensor()) {
                const bool inner1 = impl_.isInnerBody(body1.GetID());
                const bool inner2 = impl_.isInnerBody(body2.GetID());
                if ((inner1 && !body2.IsDynamic()) || (inner2 && !body1.IsDynamic())) {
                    return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
                }
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
            // Personaje contra algo solido: sus eventos salen de sus propios contactos.
            if (!body1.IsSensor() && !body2.IsSensor() &&
                (impl_.isInnerBody(body1.GetID()) || impl_.isInnerBody(body2.GetID()))) {
                return;
            }

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

    bool isInnerBody(const JPH::BodyID& id) const {
        return !inner_bodies.empty() && inner_bodies.contains(id.GetIndexAndSequenceNumber());
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
    // Geometria estatica sin entidad (setStaticMesh): los datos se quedan
    // entre mundos fisicos; el cuerpo solo existe con el mundo creado.
    struct StaticMesh {
        Vec3 origin{};
        std::vector<Vec3> triangles;
        int layer = 0;
        JPH::BodyID body;
    };
    std::unordered_map<std::uint64_t, StaticMesh> static_meshes;
    assets::AssetManager* asset_manager = nullptr;

    std::unordered_map<entt::entity, BodyEntry> entries;
    std::unordered_map<std::uint32_t, entt::entity> body_entities;

    // --- Ragdolls (ecs::Ragdoll): una capsula por hueso y sus articulaciones ---
    struct RagdollEntry {
        std::shared_ptr<ecs::RagdollRuntime> runtime;
        std::vector<JPH::BodyID> bodies;
        std::vector<JPH::Ref<JPH::Constraint>> constraints;
        std::vector<Quat> offset;       // giro del cuerpo = giro del hueso * offset
        std::vector<float> half;        // del hueso al centro, por el eje Y del cuerpo
        std::vector<float> cylinder;    // medio cilindro de la capsula
        std::vector<float> radius;
        std::vector<Vec3> previous_position, current_position;
        std::vector<Quat> previous_rotation, current_rotation;
        bool follow = true;
        Vec3 root_offset{};             // de la entidad a la raiz (horizontal)
        float ground_gap = 0.0f;        // del punto mas bajo del cuerpo a la entidad
    };
    std::unordered_map<entt::entity, RagdollEntry> ragdolls;

    void destroyRagdoll(RagdollEntry& r, float blend_out) {
        for (JPH::Ref<JPH::Constraint>& c : r.constraints) {
            if (system && c) system->RemoveConstraint(c);
        }
        r.constraints.clear();
        for (const JPH::BodyID& id : r.bodies) {
            if (id.IsInvalid() || !system) continue;
            body_entities.erase(id.GetIndexAndSequenceNumber());
            bodies().RemoveBody(id);
            bodies().DestroyBody(id);
        }
        r.bodies.clear();
        if (r.runtime) {
            r.runtime->simulating = false;
            r.runtime->blend_out = r.runtime->blend_out_total = std::max(blend_out, 0.0f);
            r.runtime->pushes.clear();
        }
    }

    static Quat axisRotation(const Vec3& from, const Vec3& to) {
        const float d = core::dot(from, to);
        if (d < -0.9999f) return Quat{1.0f, 0.0f, 0.0f, 0.0f};
        const Vec3 c = core::cross(from, to);
        return core::normalize(Quat{c.x, c.y, c.z, 1.0f + d});
    }

    void createRagdoll(ecs::World& w, entt::entity handle, const ecs::Ragdoll& rag,
                       const std::shared_ptr<ecs::RagdollRuntime>& runtime) {
        ecs::RagdollRuntime& rt = *runtime;
        RagdollEntry r;
        r.runtime = runtime;
        r.follow = rag.follow_entity;
        const ecs::Entity entity = w.wrap(handle);
        const int layer = entity.tryGet<ecs::EntityInfo>() != nullptr ? entity.get<ecs::EntityInfo>().layer : 0;
        const Vec3 kY{0.0f, 1.0f, 0.0f};
        const float dt = std::max(rt.pose_dt, 1e-3f);
        std::vector<Vec3> dirs(rt.bones.size());
        float lowest = 1e30f;
        for (std::size_t k = 0; k < rt.bones.size(); ++k) {
            const ecs::RagdollRuntime::Bone& b = rt.bones[k];
            Vec3 origin{};
            Quat bone_rotation{};
            Vec3 scale{};
            ecs::decomposeMatrix(b.world, origin, bone_rotation, scale);
            bone_rotation = core::normalize(bone_rotation);
            Vec3 axis = b.tip - origin;
            const float length = std::max(core::length(axis), 0.02f);
            const Vec3 dir = core::length(axis) > 1e-5f ? axis * (1.0f / core::length(axis)) : ecs::quatRotate(bone_rotation, kY);
            dirs[k] = dir;
            const float radius = std::clamp(b.radius, 0.01f, std::max(length * 0.5f, 0.01f));
            const float half_cylinder = std::max(length * 0.5f - radius, 0.005f);
            const Quat body_rotation = axisRotation(kY, dir);
            const Vec3 center = origin + dir * (length * 0.5f);
            JPH::RefConst<JPH::Shape> shape = new JPH::CapsuleShape(half_cylinder, radius);

            Vec3 linear{};
            Vec3 angular{};
            if (rag.inherit_velocity && rt.has_previous) {
                Vec3 origin_p{};
                Quat rotation_p{};
                Vec3 scale_p{};
                ecs::decomposeMatrix(b.previous, origin_p, rotation_p, scale_p);
                rotation_p = core::normalize(rotation_p);
                const Vec3 dir_local = ecs::quatRotate(ecs::quatConjugate(bone_rotation), dir);
                const Vec3 center_p = origin_p + ecs::quatRotate(rotation_p, dir_local) * (length * 0.5f);
                linear = (center - center_p) * (1.0f / dt);
                Quat dq = ecs::quatMultiply(bone_rotation, ecs::quatConjugate(rotation_p));
                if (dq.w < 0.0f) dq = Quat{-dq.x, -dq.y, -dq.z, -dq.w};
                const float angle = 2.0f * std::acos(std::clamp(dq.w, -1.0f, 1.0f));
                const float s = std::sqrt(std::max(1.0f - dq.w * dq.w, 0.0f));
                if (s > 1e-5f) angular = Vec3{dq.x / s, dq.y / s, dq.z / s} * (angle / dt);
                // Un teletransporte no es velocidad.
                if (core::length(linear) > 40.0f) linear = linear * (40.0f / core::length(linear));
                if (core::length(angular) > 40.0f) angular = angular * (40.0f / core::length(angular));
            }
            const JPH::BodyID id = createBody(shape, center, body_rotation, JPH::EMotionType::Dynamic, objectLayer(layer, true),
                                              handle, false, [&](JPH::BodyCreationSettings& s) {
                s.mFriction = rag.friction;
                s.mRestitution = 0.0f;
                s.mLinearDamping = std::max(rag.damping, 0.0f);
                s.mAngularDamping = std::max(rag.damping, 0.0f) + 0.05f;
                s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                s.mMassPropertiesOverride.mMass = std::max(b.mass, 0.05f);
                s.mMotionQuality = JPH::EMotionQuality::LinearCast;  // las capsulas finas no atraviesan el suelo
                s.mLinearVelocity = toJolt(linear);
                s.mAngularVelocity = toJolt(angular);
            });
            r.bodies.push_back(id);
            r.offset.push_back(ecs::quatMultiply(ecs::quatConjugate(bone_rotation), body_rotation));
            r.half.push_back(length * 0.5f);
            r.cylinder.push_back(half_cylinder);
            r.radius.push_back(radius);
            r.previous_position.push_back(center);
            r.current_position.push_back(center);
            r.previous_rotation.push_back(body_rotation);
            r.current_rotation.push_back(body_rotation);
            lowest = std::min(lowest, center.y - half_cylinder * std::abs(dir.y) - radius);
            rt.bones[k].sim_position = origin;
            rt.bones[k].sim_rotation = bone_rotation;
        }
        // Articulaciones: en el origen de cada hueso, con su padre.
        for (std::size_t k = 0; k < rt.bones.size(); ++k) {
            const int p = rt.bones[k].parent;
            if (p < 0 || r.bodies[k].IsInvalid() || r.bodies[static_cast<std::size_t>(p)].IsInvalid()) continue;
            const Vec3 anchor = rt.bones[k].sim_position;
            const Vec3 twist = dirs[k];
            const Vec3 pick = std::abs(twist.y) < 0.9f ? kY : Vec3{1.0f, 0.0f, 0.0f};
            const Vec3 plane = core::normalize(core::cross(twist, pick));
            JPH::SwingTwistConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPosition1 = s.mPosition2 = JPH::RVec3(toJolt(anchor));
            s.mTwistAxis1 = s.mTwistAxis2 = toJolt(twist);
            s.mPlaneAxis1 = s.mPlaneAxis2 = toJolt(plane);
            const float swing = std::clamp(rt.bones[k].swing, 1.0f, 179.0f) * core::kPi / 180.0f;
            const float twist_angle = std::clamp(rt.bones[k].twist, 0.5f, 179.0f) * core::kPi / 180.0f;
            s.mNormalHalfConeAngle = swing;
            s.mPlaneHalfConeAngle = swing;
            s.mTwistMinAngle = -twist_angle;
            s.mTwistMaxAngle = twist_angle;
            s.mMaxFrictionTorque = std::max(rag.joint_friction, 0.0f) * rt.bones[k].mass;
            JPH::TwoBodyConstraint* c =
                bodies().CreateConstraint(&s, r.bodies[static_cast<std::size_t>(p)], r.bodies[k]);
            if (c == nullptr) continue;
            system->AddConstraint(c);
            r.constraints.emplace_back(c);
        }
        const Vec3 entity_position = entity.worldPosition();
        const Vec3 root = rt.bones.front().sim_position;
        r.root_offset = Vec3{root.x - entity_position.x, 0.0f, root.z - entity_position.z};
        r.ground_gap = lowest - entity_position.y;
        rt.simulating = true;
        rt.sim_ready = true;
        rt.blend_out = 0.0f;
        std::cout << "[Fisica] Ragdoll de " << entity.name() << ": " << r.bodies.size() << " cuerpos, "
                  << r.constraints.size() << " articulaciones" << std::endl;
        ragdolls[handle] = std::move(r);
    }

    void syncRagdolls(ecs::World& w, bool simulate) {
        entt::registry& registry = w.registry();
        for (auto it = ragdolls.begin(); it != ragdolls.end();) {
            const entt::entity h = it->first;
            const ecs::Ragdoll* rag = registry.valid(h) ? registry.try_get<ecs::Ragdoll>(h) : nullptr;
            const bool keep = simulate && rag != nullptr && rag->active && w.wrap(h).activeInHierarchy() &&
                              rag->runtime.ptr == it->second.runtime;
            if (keep) {
                ++it;
                continue;
            }
            destroyRagdoll(it->second, rag != nullptr ? rag->blend_out : 0.0f);
            it = ragdolls.erase(it);
        }
        if (!simulate) return;
        for (const entt::entity h : registry.view<ecs::Ragdoll>()) {
            ecs::Ragdoll& rag = registry.get<ecs::Ragdoll>(h);
            const std::shared_ptr<ecs::RagdollRuntime>& rt = rag.runtime.ptr;
            if (!rag.active) {
                if (rt) rt->pushes.clear();
                continue;
            }
            if (ragdolls.contains(h) || !w.wrap(h).activeInHierarchy()) continue;
            // La animacion todavia no dijo donde estan los huesos: el frame que viene.
            if (!rt || !rt->pose_ready || rt->bones.empty()) continue;
            createRagdoll(w, h, rag, rt);
        }
        // Empujones pedidos desde Lua.
        for (auto& [h, r] : ragdolls) {
            if (!r.runtime) continue;
            for (const ecs::RagdollRuntime::Push& push : r.runtime->pushes) {
                std::size_t k = 0;
                if (push.bone >= 0 && static_cast<std::size_t>(push.bone) < r.bodies.size()) {
                    k = static_cast<std::size_t>(push.bone);
                } else if (push.at_point) {
                    float best = 1e30f;
                    for (std::size_t i = 0; i < r.current_position.size(); ++i) {
                        const float dd = core::length(r.current_position[i] - push.point);
                        if (dd < best) {
                            best = dd;
                            k = i;
                        }
                    }
                }
                if (k >= r.bodies.size() || r.bodies[k].IsInvalid()) continue;
                if (push.at_point) {
                    bodies().AddImpulse(r.bodies[k], toJolt(push.impulse), JPH::RVec3(toJolt(push.point)));
                } else {
                    bodies().AddImpulse(r.bodies[k], toJolt(push.impulse));
                }
            }
            r.runtime->pushes.clear();
        }
    }

    // --- Telas (physics::Cloth): un soft body de Jolt por entidad ---
    // Las particulas fijadas son vertices cinematicos (masa infinita) que se
    // llevan cada paso a su sitio en la entidad; el viento empuja segun la
    // normal de la tela (lo que le da de frente empuja mas).
    struct ClothEntry {
        JPH::BodyID body;
        std::shared_ptr<ClothRuntime> runtime;
        std::string layout;
        std::vector<Vec3> rest;              // en la entidad
        std::vector<std::uint32_t> pinned;
        std::vector<std::uint8_t> is_pinned;
        std::vector<Vec3> previous, current;  // en el mundo, por paso
        std::vector<Vec3> scratch, normals;
        core::Mat4 last_world{};
        float time = 0.0f;
        int layer = 0;
        bool collide = true;
    };
    std::unordered_map<entt::entity, ClothEntry> cloths;
    static int layerOf(const ecs::Entity& e) {
        const ecs::EntityInfo* info = e.tryGet<ecs::EntityInfo>();
        return info != nullptr ? info->layer : 0;
    }

    // Chocar = no: un filtro de grupo que no deja chocar con nada.
    class NoCollisionFilter final : public JPH::GroupFilter {
    public:
        bool CanCollide(const JPH::CollisionGroup&, const JPH::CollisionGroup&) const override { return false; }
    };
    JPH::Ref<JPH::GroupFilter> no_collision;  // se crea al usarlo (Jolt ya esta iniciado)

    void destroyCloth(ClothEntry& c) {
        if (!c.body.IsInvalid()) {
            body_entities.erase(c.body.GetIndexAndSequenceNumber());
            bodies().RemoveBody(c.body);
            bodies().DestroyBody(c.body);
        }
        c.body = JPH::BodyID();
        if (c.runtime) c.runtime->simulated = false;
    }

    void createCloth(ecs::World& w, entt::entity handle, Cloth& cloth) {
        const ecs::Entity e = w.wrap(handle);
        if (!cloth.runtime.ptr) cloth.runtime.ptr = std::make_shared<ClothRuntime>();
        ClothEntry c;
        c.runtime = cloth.runtime.ptr;
        c.layout = cloth.layoutKey();
        c.rest = clothRestPositions(cloth);
        c.layer = layerOf(e);
        c.collide = cloth.collide;
        const int nx = cloth.particlesX();
        const int ny = cloth.particlesY();
        const std::size_t count = c.rest.size();
        const core::Mat4 world = e.worldMatrix();
        c.last_world = world;
        c.current.resize(count);
        Vec3 center{};
        for (std::size_t i = 0; i < count; ++i) {
            c.current[i] = ecs::transformPoint(world, c.rest[i]);
            center = center + c.current[i];
        }
        center = center * (1.0f / static_cast<float>(count));
        c.previous = c.current;
        c.is_pinned.assign(count, 0);
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                if (!clothPinned(cloth, x, y)) continue;
                const auto i = static_cast<std::uint32_t>(y * nx + x);
                c.pinned.push_back(i);
                c.is_pinned[i] = 1;
            }
        }

        JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings;
        const float inv_mass = static_cast<float>(count) / std::max(cloth.mass, 1e-3f);
        for (std::size_t i = 0; i < count; ++i) {
            const Vec3 local = c.current[i] - center;
            shared->mVertices.push_back(JPH::SoftBodySharedSettings::Vertex(JPH::Float3(local.x, local.y, local.z),
                                                                             JPH::Float3(0.0f, 0.0f, 0.0f),
                                                                             c.is_pinned[i] ? 0.0f : inv_mass));
        }
        const std::vector<std::uint32_t> triangles = clothTriangles(cloth);
        for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
            shared->AddFace(JPH::SoftBodySharedSettings::Face(triangles[t], triangles[t + 1], triangles[t + 2]));
        }
        // Rigidez 0..1 -> compliance (inversa de la rigidez; 0 = rigida del todo).
        const float stretch = 1.0f - std::clamp(cloth.stiffness, 0.0f, 1.0f);
        const float edge = stretch * stretch * 1e-2f;
        const float bend_amount = std::clamp(cloth.bending, 0.0f, 1.0f);
        const float bend = bend_amount <= 1e-3f ? FLT_MAX : (1.0f - bend_amount) * (1.0f - bend_amount) * 2.0f + 1e-6f;
        // Con particulas fijas, un tope de distancia a ellas: no se estira de mas al colgar.
        const JPH::SoftBodySharedSettings::VertexAttributes attributes(
            edge, edge, bend,
            c.pinned.empty() ? JPH::SoftBodySharedSettings::ELRAType::None
                             : JPH::SoftBodySharedSettings::ELRAType::EuclideanDistance,
            1.0f + stretch * 0.15f);
        shared->CreateConstraints(&attributes, 1, JPH::SoftBodySharedSettings::EBendType::Distance);
        shared->mVertexRadius = std::max(cloth.thickness, 0.0f);
        shared->Optimize();

        JPH::SoftBodyCreationSettings settings_(shared, JPH::RVec3(toJolt(center)), JPH::Quat::sIdentity(),
                                                objectLayer(c.layer, true));
        settings_.mNumIterations = static_cast<JPH::uint32>(std::clamp(cloth.iterations, 1, 32));
        settings_.mLinearDamping = std::max(cloth.damping, 0.0f);
        settings_.mFriction = std::clamp(cloth.friction, 0.0f, 1.0f);
        settings_.mGravityFactor = cloth.gravity_scale;
        settings_.mUserData = entityToUserData(handle);
        if (!cloth.collide) {
            if (!no_collision) no_collision = new NoCollisionFilter;
            settings_.mCollisionGroup.SetGroupFilter(no_collision);
        }
        c.body = bodies().CreateAndAddSoftBody(settings_, JPH::EActivation::Activate);
        if (c.body.IsInvalid()) {
            std::cerr << "[Fisica] Tela de " << e.name() << ": no hay sitio para mas cuerpos (max_bodies)\n";
            return;
        }
        body_entities[c.body.GetIndexAndSequenceNumber()] = handle;
        c.runtime->positions = c.current;
        c.runtime->simulated = true;
        ++c.runtime->version;
        cloths[handle] = std::move(c);
    }

    void syncCloths(ecs::World& w, bool simulate) {
        auto& registry = w.registry();
        for (auto it = cloths.begin(); it != cloths.end();) {
            const entt::entity h = it->first;
            const Cloth* cloth = registry.valid(h) ? registry.try_get<Cloth>(h) : nullptr;
            const bool keep = cloth != nullptr && w.wrap(h).activeInHierarchy() && cloth->layoutKey() == it->second.layout &&
                              cloth->collide == it->second.collide && layerOf(w.wrap(h)) == it->second.layer &&
                              cloth->runtime.ptr == it->second.runtime;
            if (keep) {
                ++it;
                continue;
            }
            destroyCloth(it->second);
            it = cloths.erase(it);
        }
        if (!simulate) return;
        for (const entt::entity h : registry.view<Cloth>()) {
            if (cloths.contains(h) || !w.wrap(h).activeInHierarchy()) continue;
            createCloth(w, h, registry.get<Cloth>(h));
        }
    }

    // Antes de cada paso: fijadas a su sitio, viento, empujones y reinicios.
    void preStepCloths(ecs::World& w, float dt) {
        auto& registry = w.registry();
        for (auto& [h, c] : cloths) {
            if (c.body.IsInvalid() || !registry.valid(h)) continue;
            const Cloth* cloth = registry.try_get<Cloth>(h);
            if (cloth == nullptr) continue;
            const core::Mat4 world = w.wrap(h).worldMatrix();
            ClothRuntime& rt = *c.runtime;
            const bool reset = rt.reset_requested;
            const Vec3 push = rt.pending_velocity;
            rt.reset_requested = false;
            rt.pending_velocity = Vec3{};
            c.time += dt;
            // Viento con rachas: un poco de ruido en el tiempo y en el espacio.
            const float gust = 1.0f + cloth->turbulence * (0.6f * std::sin(c.time * 1.9f) + 0.4f * std::sin(c.time * 4.3f + 1.3f));
            const Vec3 wind = cloth->wind * std::max(gust, 0.0f);
            const bool windy = core::length(cloth->wind) > 1e-4f && cloth->air_drag > 0.0f;
            bool moved = reset || core::length(push) > 0.0f || windy;
            for (int k = 0; k < 16 && !moved; ++k) moved = world.m[k / 4][k % 4] != c.last_world.m[k / 4][k % 4];
            c.last_world = world;
            if (moved) bodies().ActivateBody(c.body);
            JPH::BodyLockWrite lock(system->GetBodyLockInterface(), c.body);
            if (!lock.Succeeded()) continue;
            JPH::Body& body = lock.GetBody();
            auto* motion = static_cast<JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
            motion->SetNumIterations(static_cast<JPH::uint32>(std::clamp(cloth->iterations, 1, 32)));
            motion->SetLinearDamping(std::max(cloth->damping, 0.0f));
            motion->SetGravityFactor(cloth->gravity_scale);
            body.SetFriction(std::clamp(cloth->friction, 0.0f, 1.0f));
            const JPH::RMat44 com = body.GetCenterOfMassTransform();
            const JPH::RMat44 to_body = com.InversedRotationTranslation();
            JPH::Array<JPH::SoftBodyVertex>& vertices = motion->GetVertices();
            if (vertices.size() != c.rest.size()) continue;
            if (windy) {
                c.scratch.resize(vertices.size());
                for (std::size_t i = 0; i < vertices.size(); ++i) {
                    c.scratch[i] = fromJolt(JPH::Vec3(com * vertices[i].mPosition));
                }
                clothNormals(*cloth, c.scratch, c.normals);
            }
            const float jiggle = cloth->turbulence * 0.35f;
            const int nx = cloth->particlesX();
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                JPH::SoftBodyVertex& v = vertices[i];
                if (c.is_pinned[i] || reset) {
                    const JPH::Vec3 target = JPH::Vec3(to_body * JPH::RVec3(toJolt(ecs::transformPoint(world, c.rest[i]))));
                    if (reset) {
                        v.mPosition = target;
                        v.mVelocity = JPH::Vec3::sZero();
                    } else {
                        // Jolt integra la velocidad tambien de los cinematicos:
                        // con ella llegan justo al objetivo al final del paso.
                        v.mVelocity = (target - v.mPosition) / std::max(dt, 1e-4f);
                    }
                    if (c.is_pinned[i]) continue;
                }
                if (v.mInvMass <= 0.0f) continue;
                if (push.x != 0.0f || push.y != 0.0f || push.z != 0.0f) v.mVelocity += toJolt(push);
                if (windy) {
                    // Rachas locales: cada zona de la tela lleva un poco de retraso.
                    const float px = static_cast<float>(static_cast<int>(i) % nx);
                    const float local = 1.0f + jiggle * std::sin(c.time * 3.1f - px * 0.45f + static_cast<float>(i / nx) * 0.3f);
                    const Vec3 relative = wind * local - fromJolt(v.mVelocity);
                    const Vec3& n = c.normals[i];
                    const float pressure = core::dot(relative, n);
                    // Empuje por la cara que da al viento, y un poco de arrastre tangencial.
                    const Vec3 accel = n * (pressure * cloth->air_drag * 0.5f) + relative * (cloth->air_drag * 0.05f);
                    v.mVelocity += toJolt(accel * dt);
                }
            }
        }
    }

    // --- Cuerpos blandos (physics::SoftBody): malla cerrada con presion ---
    struct SoftEntry {
        JPH::BodyID body;
        std::shared_ptr<SoftBodyRuntime> runtime;
        std::string layout;
        std::vector<Vec3> rest;  // en la entidad
        std::vector<Vec3> previous, current;
        int layer = 0;
        bool collide = true;
    };
    std::unordered_map<entt::entity, SoftEntry> soft_bodies;

    void destroySoftBody(SoftEntry& s) {
        if (!s.body.IsInvalid()) {
            body_entities.erase(s.body.GetIndexAndSequenceNumber());
            bodies().RemoveBody(s.body);
            bodies().DestroyBody(s.body);
        }
        s.body = JPH::BodyID();
        if (s.runtime) s.runtime->simulated = false;
    }

    void createSoftBody(ecs::World& w, entt::entity handle, SoftBody& soft) {
        const ecs::Entity e = w.wrap(handle);
        if (!soft.runtime.ptr) soft.runtime.ptr = std::make_shared<SoftBodyRuntime>();
        SoftEntry s;
        s.runtime = soft.runtime.ptr;
        s.layout = soft.layoutKey();
        s.layer = layerOf(e);
        s.collide = soft.collide;
        const SoftBodyMesh mesh = softBodyMesh(soft);
        s.rest = mesh.particles;
        const core::Mat4 world = e.worldMatrix();
        const std::size_t count = s.rest.size();
        s.current.resize(count);
        Vec3 center{};
        for (std::size_t i = 0; i < count; ++i) {
            s.current[i] = ecs::transformPoint(world, s.rest[i]);
            center = center + s.current[i];
        }
        center = center * (1.0f / static_cast<float>(count));
        s.previous = s.current;
        // Volumen y area en reposo (para la presion).
        float volume = 0.0f, area = 0.0f;
        for (std::size_t t = 0; t + 2 < mesh.triangles.size(); t += 3) {
            const Vec3 a = s.current[mesh.triangles[t]] - center;
            const Vec3 b = s.current[mesh.triangles[t + 1]] - center;
            const Vec3 c = s.current[mesh.triangles[t + 2]] - center;
            volume += core::dot(a, core::cross(b, c)) / 6.0f;
            area += core::length(core::cross(b - a, c - a)) * 0.5f;
        }
        volume = std::max(std::abs(volume), 1e-6f);
        area = std::max(area, 1e-6f);

        JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings;
        const float inv_mass = static_cast<float>(count) / std::max(soft.mass, 1e-3f);
        for (std::size_t i = 0; i < count; ++i) {
            const Vec3 local = s.current[i] - center;
            shared->mVertices.push_back(JPH::SoftBodySharedSettings::Vertex(JPH::Float3(local.x, local.y, local.z),
                                                                             JPH::Float3(0.0f, 0.0f, 0.0f), inv_mass));
        }
        for (std::size_t t = 0; t + 2 < mesh.triangles.size(); t += 3) {
            shared->AddFace(JPH::SoftBodySharedSettings::Face(mesh.triangles[t], mesh.triangles[t + 1], mesh.triangles[t + 2]));
        }
        const float soft_amount = 1.0f - std::clamp(soft.stiffness, 0.0f, 1.0f);
        const float edge = soft_amount * soft_amount * 5e-3f + 1e-7f;
        const float bend = soft_amount * soft_amount * 0.5f + 1e-5f;  // guarda la forma (no se arruga como una tela)
        const JPH::SoftBodySharedSettings::VertexAttributes attributes(edge, edge, bend);
        shared->CreateConstraints(&attributes, 1, JPH::SoftBodySharedSettings::EBendType::Distance);
        shared->mVertexRadius = std::max(soft.thickness, 0.0f);
        shared->Optimize();

        JPH::SoftBodyCreationSettings settings_(shared, JPH::RVec3(toJolt(center)), JPH::Quat::sIdentity(),
                                                objectLayer(s.layer, true));
        settings_.mNumIterations = static_cast<JPH::uint32>(std::clamp(soft.iterations, 1, 32));
        settings_.mLinearDamping = std::max(soft.damping, 0.0f);
        settings_.mFriction = std::clamp(soft.friction, 0.0f, 1.0f);
        settings_.mRestitution = std::clamp(soft.restitution, 0.0f, 1.0f);
        settings_.mGravityFactor = soft.gravity_scale;
        // Presion (P = nRT / V): en reposo empuja hacia fuera `pressure` veces
        // lo que hace falta para aguantar su propio peso sobre su superficie.
        const float rest_pressure = std::max(soft.pressure, 0.0f) * std::max(soft.mass, 1e-3f) * 9.81f / area * 4.0f;
        settings_.mPressure = rest_pressure * volume;
        settings_.mUserData = entityToUserData(handle);
        if (!soft.collide) {
            if (!no_collision) no_collision = new NoCollisionFilter;
            settings_.mCollisionGroup.SetGroupFilter(no_collision);
        }
        s.body = bodies().CreateAndAddSoftBody(settings_, JPH::EActivation::Activate);
        if (s.body.IsInvalid()) {
            std::cerr << "[Fisica] Cuerpo blando de " << e.name() << ": no hay sitio para mas cuerpos (max_bodies)\n";
            return;
        }
        body_entities[s.body.GetIndexAndSequenceNumber()] = handle;
        s.runtime->positions = s.current;
        s.runtime->simulated = true;
        ++s.runtime->version;
        soft_bodies[handle] = std::move(s);
    }

    void syncSoftBodies(ecs::World& w, bool simulate) {
        auto& registry = w.registry();
        for (auto it = soft_bodies.begin(); it != soft_bodies.end();) {
            const entt::entity h = it->first;
            const SoftBody* soft = registry.valid(h) ? registry.try_get<SoftBody>(h) : nullptr;
            const bool keep = soft != nullptr && w.wrap(h).activeInHierarchy() && soft->layoutKey() == it->second.layout &&
                              soft->collide == it->second.collide && layerOf(w.wrap(h)) == it->second.layer &&
                              soft->runtime.ptr == it->second.runtime;
            if (keep) {
                ++it;
                continue;
            }
            destroySoftBody(it->second);
            it = soft_bodies.erase(it);
        }
        if (!simulate) return;
        for (const entt::entity h : registry.view<SoftBody>()) {
            if (soft_bodies.contains(h) || !w.wrap(h).activeInHierarchy()) continue;
            createSoftBody(w, h, registry.get<SoftBody>(h));
        }
    }

    void preStepSoftBodies(ecs::World& w) {
        auto& registry = w.registry();
        for (auto& [h, s] : soft_bodies) {
            if (s.body.IsInvalid() || !registry.valid(h)) continue;
            const SoftBody* soft = registry.try_get<SoftBody>(h);
            if (soft == nullptr) continue;
            SoftBodyRuntime& rt = *s.runtime;
            const bool reset = rt.reset_requested;
            const Vec3 push = rt.pending_velocity;
            rt.reset_requested = false;
            rt.pending_velocity = Vec3{};
            if (reset || core::length(push) > 0.0f) bodies().ActivateBody(s.body);
            const core::Mat4 world = w.wrap(h).worldMatrix();
            JPH::BodyLockWrite lock(system->GetBodyLockInterface(), s.body);
            if (!lock.Succeeded()) continue;
            JPH::Body& body = lock.GetBody();
            auto* motion = static_cast<JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
            motion->SetNumIterations(static_cast<JPH::uint32>(std::clamp(soft->iterations, 1, 32)));
            motion->SetLinearDamping(std::max(soft->damping, 0.0f));
            motion->SetGravityFactor(soft->gravity_scale);
            body.SetFriction(std::clamp(soft->friction, 0.0f, 1.0f));
            body.SetRestitution(std::clamp(soft->restitution, 0.0f, 1.0f));
            if (!reset && push.x == 0.0f && push.y == 0.0f && push.z == 0.0f) continue;
            const JPH::RMat44 to_body = body.GetCenterOfMassTransform().InversedRotationTranslation();
            JPH::Array<JPH::SoftBodyVertex>& vertices = motion->GetVertices();
            if (vertices.size() != s.rest.size()) continue;
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                JPH::SoftBodyVertex& v = vertices[i];
                if (reset) {
                    v.mPosition = JPH::Vec3(to_body * JPH::RVec3(toJolt(ecs::transformPoint(world, s.rest[i]))));
                    v.mVelocity = JPH::Vec3::sZero();
                }
                v.mVelocity += toJolt(push);
            }
        }
    }

    void captureSoftBodies() {
        for (auto& [h, s] : soft_bodies) {
            if (s.body.IsInvalid()) continue;
            JPH::BodyLockRead lock(system->GetBodyLockInterface(), s.body);
            if (!lock.Succeeded()) continue;
            const JPH::Body& body = lock.GetBody();
            const auto* motion = static_cast<const JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
            const JPH::RMat44 com = body.GetCenterOfMassTransform();
            const JPH::Array<JPH::SoftBodyVertex>& vertices = motion->GetVertices();
            if (vertices.size() != s.current.size()) continue;
            s.previous.swap(s.current);
            for (std::size_t i = 0; i < vertices.size(); ++i) s.current[i] = fromJolt(JPH::Vec3(com * vertices[i].mPosition));
        }
    }

    // Posiciones al render y, si se pide, la entidad al centro del cuerpo.
    void writeSoftBodies(ecs::World& w, float alpha) {
        const float t = std::clamp(alpha, 0.0f, 1.0f);
        auto& registry = w.registry();
        for (auto& [h, s] : soft_bodies) {
            if (!s.runtime || s.current.size() != s.previous.size() || s.current.empty()) continue;
            s.runtime->positions.resize(s.current.size());
            Vec3 center{};
            for (std::size_t i = 0; i < s.current.size(); ++i) {
                s.runtime->positions[i] = core::lerp(s.previous[i], s.current[i], t);
                center = center + s.runtime->positions[i];
            }
            center = center * (1.0f / static_cast<float>(s.current.size()));
            s.runtime->simulated = true;
            ++s.runtime->version;
            const SoftBody* soft = registry.valid(h) ? registry.try_get<SoftBody>(h) : nullptr;
            if (soft != nullptr && soft->follow_entity) w.wrap(h).setWorldPosition(center);
        }
    }

    void captureCloths() {
        for (auto& [h, c] : cloths) {
            if (c.body.IsInvalid()) continue;
            JPH::BodyLockRead lock(system->GetBodyLockInterface(), c.body);
            if (!lock.Succeeded()) continue;
            const JPH::Body& body = lock.GetBody();
            const auto* motion = static_cast<const JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
            const JPH::RMat44 com = body.GetCenterOfMassTransform();
            const JPH::Array<JPH::SoftBodyVertex>& vertices = motion->GetVertices();
            if (vertices.size() != c.current.size()) continue;
            c.previous.swap(c.current);
            for (std::size_t i = 0; i < vertices.size(); ++i) c.current[i] = fromJolt(JPH::Vec3(com * vertices[i].mPosition));
        }
    }

    void writeCloths(float alpha) {
        const float t = std::clamp(alpha, 0.0f, 1.0f);
        for (auto& [h, c] : cloths) {
            if (!c.runtime || c.current.size() != c.previous.size()) continue;
            c.runtime->positions.resize(c.current.size());
            for (std::size_t i = 0; i < c.current.size(); ++i) c.runtime->positions[i] = core::lerp(c.previous[i], c.current[i], t);
            c.runtime->simulated = true;
            ++c.runtime->version;
        }
    }

    void captureRagdolls() {
        for (auto& [h, r] : ragdolls) {
            for (std::size_t k = 0; k < r.bodies.size(); ++k) {
                if (r.bodies[k].IsInvalid()) continue;
                r.previous_position[k] = r.current_position[k];
                r.previous_rotation[k] = r.current_rotation[k];
                JPH::RVec3 p;
                JPH::Quat q;
                bodies().GetPositionAndRotation(r.bodies[k], p, q);
                r.current_position[k] = fromJolt(JPH::Vec3(p));
                r.current_rotation[k] = fromJolt(q.Normalized());
            }
        }
    }

    // La pose de los huesos (interpolada como los cuerpos) y, si se pide, la
    // entidad va con el cuerpo.
    void writeRagdolls(ecs::World& w, float alpha) {
        const Vec3 kY{0.0f, 1.0f, 0.0f};
        const float t = std::clamp(alpha, 0.0f, 1.0f);
        for (auto& [h, r] : ragdolls) {
            if (!r.runtime || r.runtime->bones.size() != r.bodies.size()) continue;
            float lowest = 1e30f;
            for (std::size_t k = 0; k < r.bodies.size(); ++k) {
                const Vec3 center = core::lerp(r.previous_position[k], r.current_position[k], t);
                const Quat body_rotation = core::slerp(r.previous_rotation[k], r.current_rotation[k], t);
                const Vec3 axis = ecs::quatRotate(body_rotation, kY);
                ecs::RagdollRuntime::Bone& b = r.runtime->bones[k];
                b.sim_rotation = core::normalize(ecs::quatMultiply(body_rotation, ecs::quatConjugate(r.offset[k])));
                b.sim_position = center - axis * r.half[k];
                lowest = std::min(lowest, center.y - r.cylinder[k] * std::abs(axis.y) - r.radius[k]);
            }
            r.runtime->sim_ready = true;
            if (!r.follow || !w.valid(h)) continue;
            ecs::Entity e = w.wrap(h);
            const Vec3 root = r.runtime->bones.front().sim_position;
            e.setWorldPosition(Vec3{root.x - r.root_offset.x, lowest - r.ground_gap, root.z - r.root_offset.z});
        }
    }
    std::map<MeshKey, JPH::RefConst<JPH::Shape>> mesh_shapes;
    // Mallas creadas por codigo (MeshRenderer::mesh): por malla y version.
    struct RuntimeShapeKey {
        const ecs::Mesh* mesh = nullptr;
        std::uint64_t version = 0;
        bool convex = false;
        const JPH::PhysicsMaterial* material = nullptr;
        bool operator<(const RuntimeShapeKey& o) const {
            return std::tie(mesh, version, convex, material) < std::tie(o.mesh, o.version, o.convex, o.material);
        }
    };
    std::map<RuntimeShapeKey, JPH::RefConst<JPH::Shape>> runtime_shapes;
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

    // La malla creada por codigo del MeshRenderer de la entidad, si tiene.
    static const ecs::Mesh* runtimeMeshOf(ecs::Entity entity) {
        const ecs::MeshRenderer* renderer = entity.tryGet<ecs::MeshRenderer>();
        return renderer != nullptr ? renderer->mesh.get() : nullptr;
    }
    // Lo que identifica la malla del MeshCollider (si cambia, se rehace).
    const void* meshIdentity(ecs::Entity entity) const {
        if (const ecs::Mesh* runtime = runtimeMeshOf(entity)) return runtime;
        return meshOf(entity);
    }

    JPH::RefConst<JPH::Shape> runtimeMeshShape(const ecs::Mesh& mesh, bool convex, const JPH::PhysicsMaterial* material,
                                               const std::string& name) {
        // Las versiones viejas de esta malla ya no sirven.
        for (auto it = runtime_shapes.begin(); it != runtime_shapes.end();) {
            it = it->first.mesh == &mesh && it->first.version != mesh.version() ? runtime_shapes.erase(it) : std::next(it);
        }
        auto& slot = runtime_shapes[RuntimeShapeKey{&mesh, mesh.version(), convex, material}];
        if (slot != nullptr) return slot;
        if (!mesh.validate().empty()) return nullptr;
        if (!convex) {
            JPH::VertexList vertices;
            vertices.reserve(mesh.vertices.size());
            for (const Vec3& p : mesh.vertices) vertices.push_back(JPH::Float3(p.x, p.y, p.z));
            JPH::IndexedTriangleList triangles;
            const std::vector<std::uint32_t> all = mesh.allTriangles();
            triangles.reserve(all.size() / 3);
            for (std::size_t i = 0; i + 2 < all.size(); i += 3) triangles.push_back(JPH::IndexedTriangle(all[i], all[i + 1], all[i + 2], 0));
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
        JPH::Array<JPH::Vec3> points;
        const std::size_t stride = std::max<std::size_t>(1, mesh.vertices.size() / 4000);
        for (std::size_t i = 0; i < mesh.vertices.size(); i += stride) points.push_back(toJolt(mesh.vertices[i]));
        JPH::ConvexHullShapeSettings hull(points, JPH::cDefaultConvexRadius, material);
        const JPH::ShapeSettings::ShapeResult result = hull.Create();
        if (!result.HasError()) slot = result.Get();
        return slot;
    }

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
        if (const MeshCollider* mesh = entity.tryGet<MeshCollider>(); mesh != nullptr && runtimeMeshOf(entity) != nullptr) {
            // Malla creada por codigo: sus vertices y triangulos tal cual.
            const ecs::Mesh& runtime = *runtimeMeshOf(entity);
            bool convex = mesh->convex || (type == BodyType::Dynamic) || (mesh->material.is_trigger && type != BodyType::Static);
            if (!mesh->convex && type == BodyType::Dynamic) {
                warnOnce(entity.handle(), name, "un Mesh Collider no convexo no puede ser dinamico: se usa su envolvente convexa");
            }
            add(mesh->material, runtimeMeshShape(runtime, convex, materialFor(mesh->material), name), Vec3{}, Quat{});
            mesh_used = &runtime;
        } else if (const MeshCollider* mesh = entity.tryGet<MeshCollider>()) {
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
              terrain(r.storage<terrain::Terrain>()),
              renderer(r.storage<ecs::MeshRenderer>()) {}
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
        entt::storage_for_t<ecs::MeshRenderer>& renderer;
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
            if (const ecs::MeshRenderer* r = Pools::find(pools.renderer, entity); r != nullptr && r->mesh) {
                s.push_bits(static_cast<std::uint32_t>(r->mesh->version()));
                s.push_bits(static_cast<std::uint32_t>(r->mesh->version() >> 32));
            }
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

    // --- Vehiculos ---
    struct VehicleEntry {
        JPH::Ref<JPH::VehicleConstraint> constraint;
        JPH::BodyID body;
        std::uint64_t signature = 0;
        std::vector<Uuid> visuals;
        std::vector<Vec3> visual_scales;
        float throttle = 0.0f, steering = 0.0f, brake = 0.0f, handbrake = 0.0f;
    };
    std::unordered_map<entt::entity, VehicleEntry> vehicles;

    void removeVehicle(VehicleEntry& v) {
        if (v.constraint && system) {
            system->RemoveStepListener(v.constraint.GetPtr());
            system->RemoveConstraint(v.constraint.GetPtr());
        }
        v.constraint = nullptr;
    }

    void removeVehiclesOf(const JPH::BodyID& body) {
        for (auto it = vehicles.begin(); it != vehicles.end();) {
            if (it->second.body == body) {
                removeVehicle(it->second);
                it = vehicles.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Las constraints de vehiculo de cada Vehicle con Rigidbody dinamico y
    // ruedas (WheelCollider en los hijos). Se rehacen si cambian sus datos.
    void syncVehicles(ecs::World& w) {
        std::unordered_set<entt::entity> seen;
        for (const entt::entity handle : w.registry().view<Vehicle>()) {
            const auto eit = entries.find(handle);
            if (eit == entries.end() || eit->second.type != BodyType::Dynamic || eit->second.solid.IsInvalid()) continue;
            const ecs::Entity e = w.wrap(handle);
            if (!e.activeInHierarchy()) continue;
            const Vehicle& vehicle = e.get<Vehicle>();
            // Ruedas: los WheelCollider de los hijos (sin entrar en otro vehiculo).
            std::vector<ecs::Entity> wheels;
            std::vector<ecs::Entity> stack(e.children().size());
            std::transform(e.children().begin(), e.children().end(), stack.begin(), [&](entt::entity c) { return w.wrap(c); });
            while (!stack.empty()) {
                const ecs::Entity c = stack.back();
                stack.pop_back();
                if (!c.activeSelf() || c.has<Vehicle>()) continue;
                if (c.has<WheelCollider>()) wheels.push_back(c);
                for (const entt::entity g : c.children()) stack.push_back(w.wrap(g));
            }
            if (wheels.empty()) continue;
            std::sort(wheels.begin(), wheels.end(), [](const ecs::Entity& a, const ecs::Entity& b) { return a.uuid() < b.uuid(); });

            Vec3 body_position;
            Quat body_rotation;
            Vec3 body_scale;
            ecs::decomposeMatrix(e.worldMatrix(), body_position, body_rotation, body_scale);
            body_rotation = core::normalize(body_rotation);
            const Quat inverse{-body_rotation.x, -body_rotation.y, -body_rotation.z, body_rotation.w};
            std::vector<Vec3> local(wheels.size());
            for (std::size_t i = 0; i < wheels.size(); ++i) {
                local[i] = ecs::quatRotate(inverse, wheels[i].worldPosition() - body_position);
            }

            Signature sig;
            sig.push_back(static_cast<float>(eit->second.solid.GetIndexAndSequenceNumber()));
            sig.push_back(vehicle.engine_torque);
            sig.push_back(vehicle.min_rpm);
            sig.push_back(vehicle.max_rpm);
            sig.push_back(vehicle.automatic ? 1.0f : 0.0f);
            sig.push_back(vehicle.max_pitch_roll);
            for (std::size_t i = 0; i < wheels.size(); ++i) {
                const WheelCollider& wc = wheels[i].get<WheelCollider>();
                for (const float f : {local[i].x, local[i].y, local[i].z, wc.radius, wc.width, wc.suspension_min,
                                      wc.suspension_max, wc.spring_frequency, wc.damping, wc.max_steer_angle,
                                      wc.drive ? 1.0f : 0.0f, wc.max_brake_torque, wc.max_handbrake_torque, wc.grip}) {
                    sig.push_back(std::round(f * 1000.0f));
                }
            }
            seen.insert(handle);
            VehicleEntry& entry = vehicles[handle];
            // Las ruedas visibles se leen cada vez (cambian sin rehacer la fisica).
            entry.visuals.assign(wheels.size(), Uuid{});
            entry.visual_scales.assign(wheels.size(), Vec3{1.0f, 1.0f, 1.0f});
            for (std::size_t i = 0; i < wheels.size(); ++i) {
                entry.visuals[i] = wheels[i].get<WheelCollider>().visual;
                if (const ecs::Entity visual = w.find(entry.visuals[i]); visual.valid()) {
                    Vec3 p;
                    Quat r;
                    ecs::decomposeMatrix(visual.worldMatrix(), p, r, entry.visual_scales[i]);
                }
            }
            if (entry.constraint && entry.signature == sig.hash) continue;
            removeVehicle(entry);
            entry.signature = sig.hash;
            entry.body = eit->second.solid;

            JPH::VehicleConstraintSettings settings;
            settings.mUp = JPH::Vec3(0.0f, 1.0f, 0.0f);
            settings.mForward = JPH::Vec3(0.0f, 0.0f, -1.0f);  // -Z es el frente en el motor
            settings.mMaxPitchRollAngle = JPH::DegreesToRadians(std::clamp(vehicle.max_pitch_roll, 1.0f, 180.0f));
            for (std::size_t i = 0; i < wheels.size(); ++i) {
                const WheelCollider& wc = wheels[i].get<WheelCollider>();
                JPH::WheelSettingsWV* ws = new JPH::WheelSettingsWV;
                ws->mPosition = toJolt(local[i]);
                ws->mWheelForward = JPH::Vec3(0.0f, 0.0f, -1.0f);
                ws->mRadius = std::max(wc.radius, 0.02f);
                ws->mWidth = std::max(wc.width, 0.01f);
                ws->mSuspensionMinLength = std::max(wc.suspension_min, 0.0f);
                ws->mSuspensionMaxLength = std::max(wc.suspension_max, ws->mSuspensionMinLength + 0.01f);
                ws->mSuspensionSpring.mFrequency = std::max(wc.spring_frequency, 0.05f);
                ws->mSuspensionSpring.mDamping = std::clamp(wc.damping, 0.0f, 1.0f);
                ws->mMaxSteerAngle = JPH::DegreesToRadians(std::clamp(wc.max_steer_angle, 0.0f, 89.0f));
                ws->mMaxBrakeTorque = std::max(wc.max_brake_torque, 0.0f);
                ws->mMaxHandBrakeTorque = std::max(wc.max_handbrake_torque, 0.0f);
                for (auto& point : ws->mLongitudinalFriction.mPoints) point.mY *= wc.grip;
                for (auto& point : ws->mLateralFriction.mPoints) point.mY *= wc.grip;
                settings.mWheels.push_back(ws);
            }
            JPH::WheeledVehicleControllerSettings* controller = new JPH::WheeledVehicleControllerSettings;
            controller->mEngine.mMaxTorque = std::max(vehicle.engine_torque, 1.0f);
            controller->mEngine.mMinRPM = std::max(vehicle.min_rpm, 50.0f);
            controller->mEngine.mMaxRPM = std::max(vehicle.max_rpm, controller->mEngine.mMinRPM + 100.0f);
            controller->mTransmission.mMode = vehicle.automatic ? JPH::ETransmissionMode::Auto : JPH::ETransmissionMode::Manual;
            // Diferenciales: las ruedas motrices por parejas (izquierda/derecha a la
            // misma altura del coche); una suelta va sola.
            std::vector<int> drive;
            for (std::size_t i = 0; i < wheels.size(); ++i) {
                if (wheels[i].get<WheelCollider>().drive) drive.push_back(static_cast<int>(i));
            }
            std::vector<bool> used(wheels.size(), false);
            for (const int a : drive) {
                if (used[a]) continue;
                used[a] = true;
                int partner = -1;
                for (const int b : drive) {
                    if (!used[b] && std::abs(local[b].z - local[a].z) < 0.5f && (local[a].x < 0.0f) != (local[b].x < 0.0f)) {
                        partner = b;
                        break;
                    }
                }
                JPH::VehicleDifferentialSettings diff;
                if (partner >= 0) {
                    used[partner] = true;
                    diff.mLeftWheel = local[a].x < 0.0f ? a : partner;
                    diff.mRightWheel = local[a].x < 0.0f ? partner : a;
                } else {
                    diff.mLeftWheel = a;
                }
                controller->mDifferentials.push_back(diff);
            }
            for (auto& diff : controller->mDifferentials) {
                diff.mEngineTorqueRatio = 1.0f / static_cast<float>(controller->mDifferentials.size());
            }
            settings.mController = controller;

            JPH::ObjectLayer layer = 0;
            {
                JPH::BodyLockWrite lock(system->GetBodyLockInterface(), entry.body);
                if (!lock.Succeeded()) continue;
                layer = lock.GetBody().GetObjectLayer();
                entry.constraint = new JPH::VehicleConstraint(lock.GetBody(), settings);
            }
            entry.constraint->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterRay(layer));
            system->AddConstraint(entry.constraint.GetPtr());
            system->AddStepListener(entry.constraint.GetPtr());
        }
        for (auto it = vehicles.begin(); it != vehicles.end();) {
            if (!seen.contains(it->first)) {
                removeVehicle(it->second);
                it = vehicles.erase(it);
            } else {
                ++it;
            }
        }
    }

    void applyVehicleInputs() {
        for (auto& [handle, v] : vehicles) {
            if (!v.constraint) continue;
            auto* controller = static_cast<JPH::WheeledVehicleController*>(v.constraint->GetController());
            controller->SetDriverInput(v.throttle, v.steering, v.brake, v.handbrake);
            if (v.throttle != 0.0f || v.steering != 0.0f || v.brake != 0.0f || v.handbrake != 0.0f) {
                bodies().ActivateBody(v.body);
            }
        }
    }

    // Las ruedas visibles: la pose de Jolt respecto al cuerpo, sobre el cuerpo
    // ya interpolado (sin temblor a mas FPS que la fisica).
    void writeWheels(ecs::World& w) {
        for (auto& [handle, v] : vehicles) {
            if (!v.constraint || !w.valid(handle)) continue;
            JPH::RMat44 body_transform;
            {
                JPH::BodyLockRead lock(system->GetBodyLockInterface(), v.body);
                if (!lock.Succeeded()) continue;
                body_transform = lock.GetBody().GetWorldTransform();
            }
            const JPH::RMat44 inverse_body = body_transform.InversedRotationTranslation();
            const ecs::Entity e = w.wrap(handle);
            Vec3 p;
            Quat r;
            Vec3 s;
            ecs::decomposeMatrix(e.worldMatrix(), p, r, s);
            const core::Mat4 body_now = core::composeTrs(p, core::normalize(r), Vec3{1.0f, 1.0f, 1.0f});
            for (std::size_t i = 0; i < v.visuals.size(); ++i) {
                ecs::Entity visual = w.find(v.visuals[i]);
                if (!visual.valid()) continue;
                const JPH::RMat44 wheel = inverse_body *
                    v.constraint->GetWheelWorldTransform(static_cast<JPH::uint>(i), JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY());
                core::Mat4 local = core::Mat4::identity();
                for (int c = 0; c < 4; ++c) {
                    const JPH::Vec4 column = c < 3 ? JPH::Vec4(wheel.GetColumn3(c), 0.0f)
                                                   : JPH::Vec4(JPH::Vec3(wheel.GetTranslation()), 1.0f);
                    local.m[c][0] = column.GetX();
                    local.m[c][1] = column.GetY();
                    local.m[c][2] = column.GetZ();
                    local.m[c][3] = column.GetW();
                }
                visual.setWorldMatrix(body_now * local * core::scale(v.visual_scales[i]));
            }
        }
    }

    void destroyEntry(BodyEntry& entry) {
        if (!entry.solid.IsInvalid()) removeVehiclesOf(entry.solid);
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
            // Lo que dormia encima o al lado se despierta: si el collider cambia
            // (otra malla, otro tamano) o desaparece, no se queda flotando.
            JPH::AABox bounds = bodies().GetTransformedShape(*id).GetWorldSpaceBounds();
            bounds.ExpandBy(JPH::Vec3::sReplicate(0.25f));
            bodies().ActivateBodiesInAABox(bounds, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter());
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

    void destroyStaticBody(StaticMesh& mesh) {
        if (!system || mesh.body.IsInvalid()) return;
        // Lo que dormia encima se despierta (si no, flotaria donde estaba el suelo).
        JPH::AABox bounds = bodies().GetTransformedShape(mesh.body).GetWorldSpaceBounds();
        bounds.ExpandBy(JPH::Vec3::sReplicate(0.5f));
        bodies().ActivateBodiesInAABox(bounds, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter());
        bodies().RemoveBody(mesh.body);
        bodies().DestroyBody(mesh.body);
        mesh.body = JPH::BodyID();
    }

    void createStaticBody(StaticMesh& mesh) {
        destroyStaticBody(mesh);
        if (!system || mesh.triangles.size() < 3) return;
        JPH::TriangleList triangles;
        triangles.reserve(mesh.triangles.size() / 3);
        for (std::size_t i = 0; i + 2 < mesh.triangles.size(); i += 3) {
            const Vec3& a = mesh.triangles[i];
            const Vec3& b = mesh.triangles[i + 1];
            const Vec3& c = mesh.triangles[i + 2];
            triangles.push_back(JPH::Triangle(JPH::Float3(a.x, a.y, a.z), JPH::Float3(b.x, b.y, b.z), JPH::Float3(c.x, c.y, c.z)));
        }
        JPH::MeshShapeSettings settings(triangles);
        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.HasError()) {
            std::cerr << "[Fisica] Malla estatica no valida: " << result.GetError().c_str() << "\n";
            return;
        }
        mesh.body = createBody(result.Get(), mesh.origin, Quat{0.0f, 0.0f, 0.0f, 1.0f}, JPH::EMotionType::Static,
                               objectLayer(mesh.layer, false), entt::null, false, {});
        // Sin entidad: no da eventos de colision ni sale en las consultas de entidades.
        if (!mesh.body.IsInvalid()) body_entities.erase(mesh.body.GetIndexAndSequenceNumber());
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
            // Ragdoll cayendo: su propio collider (la capsula del personaje)
            // sale de la simulacion hasta que se apague.
            if (ragdolls.contains(handle)) continue;
            // El CharacterController es su collider (los demas se ignoran).
            if (registry.all_of<CharacterController>(handle)) continue;
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
            const void* mesh_now = it != entries.end() && has_mesh ? meshIdentity(entity) : nullptr;
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
        syncVehicles(w);
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
        writeWheels(w);
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

    // Flotacion (componente WaterBody): cada cuerpo dinamico que toca el agua
    // recibe el empuje de Jolt, con la altura, la normal y la corriente del
    // agua en su centro (la misma ola que se dibuja).
    void applyBuoyancy(ecs::World& w, float dt) {
        auto view = w.registry().view<water::WaterBody>();
        if (view.begin() == view.end()) return;
        struct Water {
            const water::WaterBody* body;
            core::Mat4 matrix;
        };
        std::vector<Water> waters;
        for (const entt::entity handle : view) {
            const ecs::Entity e = w.wrap(handle);
            const water::WaterBody& body = view.get<water::WaterBody>(handle);
            if (body.buoyancy && e.activeInHierarchy()) waters.push_back(Water{&body, e.worldMatrix()});
        }
        if (waters.empty()) return;
        const float time = water::waterTime();
        JPH::BodyInterface& bodies = system->GetBodyInterface();
        for (const auto& [handle, entry] : entries) {
            if (entry.type != BodyType::Dynamic || entry.solid.IsInvalid()) continue;
            JPH::AABox box;
            {
                JPH::BodyLockRead lock(system->GetBodyLockInterface(), entry.solid);
                if (!lock.Succeeded()) continue;
                box = lock.GetBody().GetWorldSpaceBounds();
            }
            const JPH::Vec3 center = box.GetCenter();
            for (const Water& water_body : waters) {
                const water::WaterSample sample = water::sampleWater(
                    *water_body.body, water_body.matrix, Vec3{center.GetX(), center.GetY(), center.GetZ()}, time);
                if (!sample.inside || box.mMin.GetY() >= sample.height) continue;
                bodies.ApplyBuoyancyImpulse(entry.solid, JPH::RVec3(center.GetX(), sample.height, center.GetZ()),
                                            toJolt(sample.normal), water_body.body->density * 1.4f,
                                            water_body.body->drag, water_body.body->drag * 0.5f,
                                            toJolt(sample.velocity), system->GetGravity(), dt);
                break;
            }
        }
    }

    void stepOnce(ecs::World& w) {
        const float dt = settings.fixed_step;
        moveKinematics(dt);
        stepCharacters(w, dt);
        preStepCloths(w, dt);
        preStepSoftBodies(w);
        applyBuoyancy(w, dt);
        applyVehicleInputs();
        system->Update(dt, std::max(settings.collision_steps, 1), temp_allocator.get(), job_system.get());
        ++steps;
        capturePoses();
        captureRagdolls();
        captureCloths();
        captureSoftBodies();
        processContacts();
        (void)w;
    }

    // --- Character Controller (CharacterVirtual de Jolt) ---
    // El personaje no es un cuerpo: se mueve con barridos de su capsula antes
    // de cada paso. Su "inner body" (cinematico, con la entidad en su
    // UserData) es lo que ven los rayos y los triggers; con lo solido solo
    // empuja a los dinamicos (los eventos de colision salen de los contactos
    // del propio personaje).
    struct CharacterTouch {
        entt::entity other = entt::null;
        bool touched = false;  // en este paso
        bool active = false;   // ya dio Enter
        Vec3 point{};
        Vec3 normal{};  // del personaje hacia el otro
        Vec3 velocity{};
    };
    struct CharacterEntry {
        JPH::Ref<JPH::CharacterVirtual> character;
        JPH::RefConst<JPH::Shape> standing;
        JPH::RefConst<JPH::Shape> crouched;
        std::uint64_t key = 0;
        int layer = 0;
        Vec3 position{};  // la de la entidad (la ultima leida o escrita)
        Vec3 previous_position{}, current_position{};
        float previous_yaw = 0.0f, current_yaw = 0.0f;
        bool yaw_driven = false;  // hay que escribir el giro
        Vec3 scale{1.0f, 1.0f, 1.0f};
        // Entrada
        Vec3 input{};
        bool run = false;
        bool crouch_wanted = false;
        bool crouching = false;
        bool jump_held = false;
        float jump_buffer = 0.0f;  // s que sigue pedido un salto
        float jump_height = -1.0f;
        float since_grounded = 0.0f;
        int jumps_used = 0;
        Vec3 move_velocity{};      // horizontal, relativa al suelo
        Vec3 manual_velocity{};    // modo Manual
        Vec3 pending_velocity{};   // addCharacterVelocity
        Vec3 velocity{};           // la real del ultimo paso
        std::uint32_t flags = 0;
        std::unordered_map<std::uint32_t, CharacterTouch> touches;  // por cuerpo
        std::uint64_t seen = 0;
    };
    std::unordered_map<entt::entity, CharacterEntry> characters;
    std::unordered_set<std::uint32_t> inner_bodies;  // se leen desde los hilos de Jolt (solo cambian fuera)
    JPH::CharacterVsCharacterCollisionSimple character_vs_character;

    class CharacterListener final : public JPH::CharacterContactListener {
    public:
        explicit CharacterListener(Impl& impl) : impl_(impl) {}
        void OnContactAdded(const JPH::CharacterVirtual* character, const JPH::BodyID& body, const JPH::SubShapeID&,
                            JPH::RVec3Arg position, JPH::Vec3Arg normal, JPH::CharacterContactSettings& settings) override {
            touch(character, body, position, normal, settings);
        }
        void OnContactPersisted(const JPH::CharacterVirtual* character, const JPH::BodyID& body, const JPH::SubShapeID&,
                                JPH::RVec3Arg position, JPH::Vec3Arg normal, JPH::CharacterContactSettings& settings) override {
            touch(character, body, position, normal, settings);
        }
        void OnCharacterContactAdded(const JPH::CharacterVirtual* character, const JPH::CharacterVirtual* other,
                                     const JPH::SubShapeID&, JPH::RVec3Arg position, JPH::Vec3Arg normal,
                                     JPH::CharacterContactSettings&) override {
            touchCharacter(character, other, position, normal);
        }
        void OnCharacterContactPersisted(const JPH::CharacterVirtual* character, const JPH::CharacterVirtual* other,
                                         const JPH::SubShapeID&, JPH::RVec3Arg position, JPH::Vec3Arg normal,
                                         JPH::CharacterContactSettings&) override {
            touchCharacter(character, other, position, normal);
        }

    private:
        CharacterEntry* entryOf(const JPH::CharacterVirtual* character) const {
            const auto it = impl_.characters.find(userDataToEntity(character->GetUserData()));
            return it != impl_.characters.end() ? &it->second : nullptr;
        }
        static void store(CharacterTouch& t, entt::entity other, JPH::RVec3Arg position, JPH::Vec3Arg normal) {
            t.other = other;
            t.touched = true;
            t.point = fromJolt(JPH::Vec3(position));
            t.normal = -fromJolt(normal);  // Jolt: hacia el personaje
        }
        void touch(const JPH::CharacterVirtual* character, const JPH::BodyID& body, JPH::RVec3Arg position,
                   JPH::Vec3Arg normal, JPH::CharacterContactSettings& settings) {
            CharacterEntry* entry = entryOf(character);
            if (entry == nullptr) return;
            ecs::World* w = impl_.world;
            const entt::entity self = userDataToEntity(character->GetUserData());
            const CharacterController* cc =
                w != nullptr && w->valid(self) ? w->registry().try_get<CharacterController>(self) : nullptr;
            settings.mCanReceiveImpulses = cc == nullptr || cc->push_rigidbodies;
            const auto it = impl_.body_entities.find(body.GetIndexAndSequenceNumber());
            if (it == impl_.body_entities.end()) return;  // geometria sin entidad
            store(entry->touches[body.GetIndexAndSequenceNumber()], it->second, position, normal);
        }
        void touchCharacter(const JPH::CharacterVirtual* character, const JPH::CharacterVirtual* other,
                            JPH::RVec3Arg position, JPH::Vec3Arg normal) {
            CharacterEntry* entry = entryOf(character);
            if (entry == nullptr || other->GetInnerBodyID().IsInvalid()) return;
            store(entry->touches[other->GetInnerBodyID().GetIndexAndSequenceNumber()],
                  userDataToEntity(other->GetUserData()), position, normal);
        }
        Impl& impl_;
    };
    CharacterListener character_listener{*this};

    // Filtro de cuerpos del personaje: ni triggers ni lo de su propia entidad.
    class CharacterBodyFilter final : public JPH::BodyFilter {
    public:
        explicit CharacterBodyFilter(std::uint64_t own) : own_(own) {}
        bool ShouldCollideLocked(const JPH::Body& body) const override {
            return !body.IsSensor() && body.GetUserData() != own_;
        }

    private:
        std::uint64_t own_;
    };

    static float yawOf(const Quat& q) {
        // Giro alrededor de Y del -Z local (el "delante" del motor).
        return std::atan2(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    }
    static Quat yawRotation(float yaw) {
        return Quat{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    }

    static std::uint64_t characterKey(const CharacterController& cc, const Vec3& scale, int layer) {
        Signature s;
        for (const float f : {cc.height, cc.radius, cc.center.x, cc.center.y, cc.center.z, cc.crouch_height, cc.skin_width,
                              static_cast<float>(layer)}) {
            s.push_back(f);
        }
        // La escala sale de descomponer la matriz (con ruido al girar): redondeada.
        for (const float f : {scale.x, scale.y, scale.z}) s.push_back(std::round(f * 1000.0f));
        return s.hash;
    }

    // Capsula (con su centro) a la escala de la entidad; agachada, con los pies en el mismo sitio.
    static JPH::RefConst<JPH::Shape> characterShape(const CharacterController& cc, const Vec3& scale, bool crouched) {
        const float radial = std::max(std::abs(scale.x), std::abs(scale.z));
        const float radius = std::max(cc.radius * radial, 0.01f);
        const float full = std::max(cc.height * std::abs(scale.y), 2.0f * radius + 0.01f);
        const float height = crouched ? std::clamp(cc.crouch_height * std::abs(scale.y), 2.0f * radius + 0.01f, full) : full;
        Vec3 center{cc.center.x * scale.x, cc.center.y * scale.y, cc.center.z * scale.z};
        center.y -= (full - height) * 0.5f;
        const float half_cylinder = std::max(height * 0.5f - radius, 0.005f);
        JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(half_cylinder, radius);
        return JPH::RotatedTranslatedShapeSettings(toJolt(center), JPH::Quat::sIdentity(), capsule).Create().Get();
    }

    void destroyCharacter(entt::entity handle, CharacterEntry& c) {
        if (!c.character) return;
        const JPH::BodyID inner = c.character->GetInnerBodyID();
        if (!inner.IsInvalid()) {
            const std::uint32_t key = inner.GetIndexAndSequenceNumber();
            body_entities.erase(key);
            inner_bodies.erase(key);
            for (auto it = pairs.begin(); it != pairs.end();) {
                if (it->second.body_a == key || it->second.body_b == key) {
                    emitExit(it->second);
                    it = pairs.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& [body, t] : c.touches) {
            if (t.active) dispatchTouch(handle, t, PhysicsEventType::CollisionExit);
        }
        c.touches.clear();
        character_vs_character.Remove(c.character);
        c.character = nullptr;  // el destructor quita el inner body
    }

    void createCharacter(ecs::World& w, entt::entity handle, const CharacterController& cc, CharacterEntry& c) {
        const ecs::Entity e = w.wrap(handle);
        Vec3 position{}, scale{};
        Quat rotation{};
        ecs::decomposeMatrix(e.worldMatrix(), position, rotation, scale);
        c.scale = scale;
        c.layer = layerOf(e);
        c.key = characterKey(cc, scale, c.layer);
        c.standing = characterShape(cc, scale, false);
        c.crouched = characterShape(cc, scale, true);
        c.crouching = false;
        c.position = c.previous_position = c.current_position = position;
        c.previous_yaw = c.current_yaw = yawOf(core::normalize(rotation));

        JPH::Ref<JPH::CharacterVirtualSettings> s = new JPH::CharacterVirtualSettings;
        s->mShape = c.standing;
        s->mInnerBodyShape = c.standing;
        s->mInnerBodyLayer = objectLayer(c.layer, true);
        s->mMaxSlopeAngle = core::radians(std::clamp(cc.slope_limit, 0.0f, 89.0f));
        s->mMass = std::max(cc.mass, 1.0f);
        s->mMaxStrength = cc.push_rigidbodies ? std::max(cc.push_strength, 0.0f) : 0.0f;
        s->mCharacterPadding = std::clamp(cc.skin_width, 0.001f, 0.2f);
        s->mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
        s->mEnhancedInternalEdgeRemoval = true;
        // Solo lo que esta por debajo del centro de la semiesfera de abajo sostiene.
        const float radial = std::max(std::abs(scale.x), std::abs(scale.z));
        const float feet = cc.center.y * scale.y - std::max(cc.height * std::abs(scale.y), 0.0f) * 0.5f;
        s->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -(feet + cc.radius * radial));
        c.character = new JPH::CharacterVirtual(s, JPH::RVec3(toJolt(position)), JPH::Quat::sIdentity(),
                                                entityToUserData(handle), system.get());
        c.character->SetListener(&character_listener);
        c.character->SetCharacterVsCharacterCollision(&character_vs_character);
        character_vs_character.Add(c.character);
        const JPH::BodyID inner = c.character->GetInnerBodyID();
        if (!inner.IsInvalid()) {
            body_entities[inner.GetIndexAndSequenceNumber()] = handle;
            inner_bodies.insert(inner.GetIndexAndSequenceNumber());
            // Que lo detecten los triggers quietos.
            JPH::BodyLockWrite lock(system->GetBodyLockInterface(), inner);
            if (lock.Succeeded()) lock.GetBody().SetCollideKinematicVsNonDynamic(true);
        }
        refreshCharacterContacts(c, handle);
    }

    void refreshCharacterContacts(CharacterEntry& c, entt::entity handle) {
        const JPH::ObjectLayer layer = objectLayer(c.layer, true);
        const CharacterBodyFilter filter(entityToUserData(handle));
        c.character->RefreshContacts(system->GetDefaultBroadPhaseLayerFilter(layer), system->GetDefaultLayerFilter(layer),
                                     filter, {}, *temp_allocator);
    }

    void syncCharacters(ecs::World& w, bool simulate) {
        auto& registry = w.registry();
        ++character_generation;
        if (simulate) {
            for (const entt::entity h : registry.view<CharacterController>()) {
                const ecs::Entity e = w.wrap(h);
                if (!e.activeInHierarchy()) continue;
                if (ragdolls.contains(h)) continue;  // cayendo como ragdoll: sin capsula
                const CharacterController& cc = registry.get<CharacterController>(h);
                const core::Mat4& m = e.worldMatrix();
                Vec3 position{}, scale{};
                Quat rotation{};
                ecs::decomposeMatrix(m, position, rotation, scale);
                CharacterEntry& c = characters[h];
                c.seen = character_generation;
                if (!c.character || c.key != characterKey(cc, scale, layerOf(e))) {
                    destroyCharacter(h, c);
                    createCharacter(w, h, cc, c);
                    continue;
                }
                // Movido desde fuera (script, editor): teletransporte.
                if (!nearlyEqual(c.position, position, 1e-4f)) {
                    c.character->SetPosition(JPH::RVec3(toJolt(position)));
                    c.position = c.previous_position = c.current_position = position;
                    refreshCharacterContacts(c, h);
                }
                if (!c.yaw_driven) c.previous_yaw = c.current_yaw = yawOf(core::normalize(rotation));
                c.character->SetMaxSlopeAngle(core::radians(std::clamp(cc.slope_limit, 0.0f, 89.0f)));
                c.character->SetMass(std::max(cc.mass, 1.0f));
                c.character->SetMaxStrength(cc.push_rigidbodies ? std::max(cc.push_strength, 0.0f) : 0.0f);
            }
        }
        for (auto it = characters.begin(); it != characters.end();) {
            if (it->second.seen != character_generation) {
                destroyCharacter(it->first, it->second);
                it = characters.erase(it);
            } else {
                ++it;
            }
        }
    }
    std::uint64_t character_generation = 0;

    // Lo que toca ahora (las banderas de Unity) a partir de sus contactos.
    static std::uint32_t contactFlags(const JPH::CharacterVirtual& ch, float slope_cos) {
        std::uint32_t flags = 0;
        for (const JPH::CharacterVirtual::Contact& k : ch.GetActiveContacts()) {
            if (k.mIsSensorB || (!k.mHadCollision && k.mDistance > ch.GetCharacterPadding() * 2.0f)) continue;
            const float y = k.mContactNormal.GetY();  // hacia el personaje
            if (y >= std::min(slope_cos, 0.7f) - 1e-3f) {
                flags |= PhysicsSystem::kCollidedBelow;
            } else if (y < -0.5f) {
                flags |= PhysicsSystem::kCollidedAbove;
            } else {
                flags |= PhysicsSystem::kCollidedSides;
            }
        }
        return flags;
    }

    // Un movimiento del personaje: velocidad durante dt, con escalones y pegado al suelo.
    void advanceCharacter(CharacterEntry& c, entt::entity handle, const CharacterController& cc, const Vec3& velocity,
                          float dt, const JPH::Vec3& gravity, bool stick) {
        JPH::CharacterVirtual& ch = *c.character;
        ch.SetLinearVelocity(toJolt(velocity));
        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mStickToFloorStepDown = stick ? JPH::Vec3(0.0f, -std::max(cc.step_offset, 0.25f), 0.0f) : JPH::Vec3::sZero();
        update.mWalkStairsStepUp = JPH::Vec3(0.0f, std::max(cc.step_offset * std::abs(c.scale.y), 0.0f), 0.0f);
        const JPH::ObjectLayer layer = objectLayer(c.layer, true);
        const CharacterBodyFilter filter(entityToUserData(handle));
        ch.ExtendedUpdate(dt, gravity, update, system->GetDefaultBroadPhaseLayerFilter(layer),
                          system->GetDefaultLayerFilter(layer), filter, {}, *temp_allocator);
        c.flags = contactFlags(ch, std::cos(core::radians(std::clamp(cc.slope_limit, 0.0f, 89.0f))));
    }

    void tryCrouch(CharacterEntry& c, entt::entity handle, bool want) {
        if (want == c.crouching) return;
        const JPH::ObjectLayer layer = objectLayer(c.layer, true);
        const CharacterBodyFilter filter(entityToUserData(handle));
        // Levantarse solo si cabe de pie.
        if (c.character->SetShape(want ? c.crouched.GetPtr() : c.standing.GetPtr(), 1.5f * c.character->GetCharacterPadding(),
                                  system->GetDefaultBroadPhaseLayerFilter(layer), system->GetDefaultLayerFilter(layer),
                                  filter, {}, *temp_allocator)) {
            c.character->SetInnerBodyShape(want ? c.crouched.GetPtr() : c.standing.GetPtr());
            c.crouching = want;
        }
    }

    static Vec3 moveTowards(const Vec3& from, const Vec3& to, float max_delta) {
        const Vec3 d = to - from;
        const float len = core::length(d);
        return len <= max_delta || len < 1e-6f ? to : from + d * (max_delta / len);
    }

    void stepCharacters(ecs::World& w, float dt) {
        if (characters.empty()) return;
        auto& registry = w.registry();
        for (auto& [h, c] : characters) {
            if (!c.character || !registry.valid(h)) continue;
            const CharacterController* ccp = registry.try_get<CharacterController>(h);
            if (ccp == nullptr) continue;
            const CharacterController& cc = *ccp;
            JPH::CharacterVirtual& ch = *c.character;
            const JPH::Vec3 up = JPH::Vec3::sAxisY();
            const Vec3 before = fromJolt(JPH::Vec3(ch.GetPosition()));
            c.previous_position = c.current_position;
            c.previous_yaw = c.current_yaw;

            if (cc.movement == CharacterMovement::Manual) {
                const Vec3 v = c.manual_velocity + c.pending_velocity;
                c.pending_velocity = Vec3{};
                advanceCharacter(c, h, cc, v, dt, system->GetGravity(), false);
            } else {
                const JPH::Vec3 gravity = system->GetGravity() * cc.gravity_scale;
                tryCrouch(c, h, c.crouch_wanted);
                ch.UpdateGroundVelocity();
                const bool grounded = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
                const JPH::Vec3 ground_velocity = ch.GetGroundVelocity();
                const float vertical = ch.GetLinearVelocity().Dot(up);
                const bool settling = vertical - ground_velocity.Dot(up) < 0.1f;
                c.since_grounded = grounded ? 0.0f : c.since_grounded + dt;
                if (grounded && settling) c.jumps_used = 0;

                // Horizontal: hacia la velocidad pedida con aceleracion (menos en el aire).
                Vec3 input{c.input.x, 0.0f, c.input.z};
                const float amount = core::length(input);
                if (amount > 1.0f) input = input * (1.0f / amount);
                const float speed = c.crouching ? cc.crouch_speed : (c.run ? cc.run_speed : cc.walk_speed);
                const Vec3 desired = input * speed;
                const float control = grounded ? 1.0f : std::clamp(cc.air_control, 0.0f, 1.0f);
                if (cc.acceleration <= 0.0f) {
                    c.move_velocity = grounded ? desired : core::lerp(c.move_velocity, desired, control);
                } else {
                    c.move_velocity = moveTowards(c.move_velocity, desired, cc.acceleration * control * dt);
                }
                c.move_velocity = c.move_velocity + Vec3{c.pending_velocity.x, 0.0f, c.pending_velocity.z};

                JPH::Vec3 v = grounded && settling ? ground_velocity : up * vertical;
                v += JPH::Vec3(0.0f, c.pending_velocity.y, 0.0f);
                c.pending_velocity = Vec3{};
                // Salto (pedido hace poco, en el suelo, en tiempo coyote o saltos que quedan).
                if (c.jump_buffer > 0.0f) {
                    const bool from_ground = (grounded || c.since_grounded <= cc.coyote_time) && c.jumps_used == 0;
                    const bool in_air = !from_ground && c.jumps_used > 0 && c.jumps_used < std::max(cc.max_jumps, 1);
                    const bool first_in_air = !from_ground && c.jumps_used == 0 && cc.max_jumps > 1;  // cayendo sin saltar
                    if (cc.max_jumps > 0 && (from_ground || in_air || first_in_air)) {
                        const float h = c.jump_height >= 0.0f ? c.jump_height : cc.jump_height;
                        const float g = std::max(gravity.Length(), 1e-3f);
                        const float base = from_ground && grounded ? ground_velocity.Dot(up) : 0.0f;
                        v = v - up * v.Dot(up) + up * (std::sqrt(2.0f * g * std::max(h, 0.0f)) + base);
                        c.jumps_used = first_in_air ? 2 : c.jumps_used + 1;
                        c.jump_buffer = 0.0f;
                        c.since_grounded = cc.coyote_time + 1.0f;
                    } else {
                        c.jump_buffer -= dt;
                    }
                }
                v += toJolt(c.move_velocity) + gravity * dt;
                const bool jumping = v.Dot(up) - ground_velocity.Dot(up) > 0.5f;
                advanceCharacter(c, h, cc, fromJolt(v), dt, gravity, cc.stick_to_floor && !jumping);
                // Contra el techo: no sigue subiendo.
                const float moved_up = (fromJolt(JPH::Vec3(ch.GetPosition())) - before).y / dt;
                if ((c.flags & PhysicsSystem::kCollidedAbove) != 0 && v.GetY() > 0.0f && moved_up < v.GetY()) {
                    ch.SetLinearVelocity(ch.GetLinearVelocity() - up * (ch.GetLinearVelocity().GetY() - std::max(moved_up, 0.0f)));
                }
                // Mirar hacia donde anda.
                if (cc.rotate_to_movement && core::length(c.move_velocity) > 0.2f && amount > 0.05f) {
                    const float target = std::atan2(-c.move_velocity.x, -c.move_velocity.z);
                    float delta = std::remainder(target - c.current_yaw, 2.0f * core::kPi);
                    const float max_turn = core::radians(std::max(cc.rotation_speed, 0.0f)) * dt;
                    delta = std::clamp(delta, -max_turn, max_turn);
                    c.current_yaw = std::remainder(c.current_yaw + delta, 2.0f * core::kPi);
                    c.yaw_driven = true;
                }
            }
            c.current_position = fromJolt(JPH::Vec3(ch.GetPosition()));
            c.velocity = (c.current_position - before) * (1.0f / dt);
        }
        processCharacterTouches();
    }

    void dispatchTouch(entt::entity self, const CharacterTouch& t, PhysicsEventType type) {
        if (world == nullptr) return;
        PhysicsEvent e;
        e.type = type;
        e.a = world->wrap(self);
        e.b = world->wrap(t.other);
        e.point = t.point;
        e.normal = t.normal;
        e.relative_velocity = t.velocity;
        e.contact_count = 1;
        e.step = steps;
        dispatch(e);
    }

    // Toques de este paso -> Enter / Stay / Exit (por cuerpo tocado).
    void processCharacterTouches() {
        for (auto& [h, c] : characters) {
            for (auto it = c.touches.begin(); it != c.touches.end();) {
                CharacterTouch& t = it->second;
                const bool gone = !body_entities.contains(it->first);
                if (t.touched && !gone) {
                    t.velocity = -c.velocity;
                    dispatchTouch(h, t, t.active ? PhysicsEventType::CollisionStay : PhysicsEventType::CollisionEnter);
                    t.active = true;
                    t.touched = false;
                    ++it;
                    continue;
                }
                if (t.active) dispatchTouch(h, t, PhysicsEventType::CollisionExit);
                it = c.touches.erase(it);
            }
        }
    }

    // Transform suave entre pasos, como los dinamicos.
    void writeCharacters(ecs::World& w, float alpha) {
        const float t = std::clamp(alpha, 0.0f, 1.0f);
        for (auto& [h, c] : characters) {
            if (!c.character || !w.valid(h)) continue;
            const Vec3 position = core::lerp(c.previous_position, c.current_position, t);
            ecs::Entity e = w.wrap(h);
            if (c.yaw_driven) {
                const float d = std::remainder(c.current_yaw - c.previous_yaw, 2.0f * core::kPi);
                Vec3 p{}, scale{};
                Quat r{};
                ecs::decomposeMatrix(e.worldMatrix(), p, r, scale);
                e.setWorldMatrix(core::composeTrs(position, yawRotation(c.previous_yaw + d * t), scale));
            } else {
                e.setWorldPosition(position);
            }
            c.position = e.worldPosition();
        }
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
        } else if (filter.ignore.valid()) {
            const auto it = characters.find(filter.ignore.handle());
            if (it != characters.end() && it->second.character) a = it->second.character->GetInnerBodyID();
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
    for (auto& [key, mesh] : d.static_meshes) {
        mesh.body = JPH::BodyID();
        d.createStaticBody(mesh);
    }
}

void PhysicsSystem::stop() {
    Impl& d = *impl_;
    if (!d.system) return;
    for (auto& [handle, ragdoll] : d.ragdolls) d.destroyRagdoll(ragdoll, 0.0f);
    d.ragdolls.clear();
    for (auto& [handle, cloth] : d.cloths) d.destroyCloth(cloth);
    d.cloths.clear();
    for (auto& [handle, soft] : d.soft_bodies) d.destroySoftBody(soft);
    d.soft_bodies.clear();
    for (auto& [handle, vehicle] : d.vehicles) d.removeVehicle(vehicle);
    d.vehicles.clear();
    d.pairs.clear();  // sin eventos Exit al parar
    for (auto& [handle, character] : d.characters) {
        character.touches.clear();
        d.destroyCharacter(handle, character);
    }
    d.characters.clear();
    d.inner_bodies.clear();
    for (auto& [handle, entry] : d.entries) {
        for (const JPH::BodyID& id : {entry.solid, entry.sensor}) {
            if (id.IsInvalid()) continue;
            d.bodies().RemoveBody(id);
            d.bodies().DestroyBody(id);
        }
    }
    d.entries.clear();
    for (auto& [key, mesh] : d.static_meshes) d.destroyStaticBody(mesh);  // los datos se quedan
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
    d.runtime_shapes.clear();
    d.contact_points.clear();
    d.queries.clear();
    d.world = nullptr;
}

void PhysicsSystem::setStaticMesh(std::uint64_t key, const Vec3& origin, std::vector<Vec3> triangles, int layer) {
    Impl& d = *impl_;
    if (triangles.size() < 3) {
        removeStaticMesh(key);
        return;
    }
    Impl::StaticMesh& mesh = d.static_meshes[key];
    mesh.origin = origin;
    mesh.triangles = std::move(triangles);
    mesh.layer = layer;
    d.createStaticBody(mesh);
}

void PhysicsSystem::removeStaticMesh(std::uint64_t key) {
    Impl& d = *impl_;
    const auto it = d.static_meshes.find(key);
    if (it == d.static_meshes.end()) return;
    d.destroyStaticBody(it->second);
    d.static_meshes.erase(it);
}

void PhysicsSystem::shiftOrigin(const core::Vec3& offset) {
    Impl& d = *impl_;
    const auto shift_matrix = [&](core::Mat4& m) {
        m.m[3][0] -= offset.x;
        m.m[3][1] -= offset.y;
        m.m[3][2] -= offset.z;
    };
    for (auto& [handle, entry] : d.entries) {
        entry.position = entry.position - offset;
        entry.previous_position = entry.previous_position - offset;
        entry.current_position = entry.current_position - offset;
        shift_matrix(entry.matrix);
    }
    for (auto& [key, mesh] : d.static_meshes) mesh.origin = mesh.origin - offset;
    for (auto& [handle, c] : d.characters) {
        c.position = c.position - offset;
        c.previous_position = c.previous_position - offset;
        c.current_position = c.current_position - offset;
        // Su inner body se mueve con los demas cuerpos (abajo).
        if (c.character) c.character->SetPosition(c.character->GetPosition() - JPH::RVec3(toJolt(offset)));
    }
    for (auto& [handle, ragdoll] : d.ragdolls) {
        for (Vec3& p : ragdoll.previous_position) p = p - offset;
        for (Vec3& p : ragdoll.current_position) p = p - offset;
    }
    for (auto& [handle, cloth] : d.cloths) {
        for (Vec3& p : cloth.previous) p = p - offset;
        for (Vec3& p : cloth.current) p = p - offset;
        shift_matrix(cloth.last_world);
        if (cloth.runtime) {
            for (Vec3& p : cloth.runtime->positions) p = p - offset;
        }
    }
    for (auto& [handle, soft] : d.soft_bodies) {
        for (Vec3& p : soft.previous) p = p - offset;
        for (Vec3& p : soft.current) p = p - offset;
        if (soft.runtime) {
            for (Vec3& p : soft.runtime->positions) p = p - offset;
        }
    }
    d.queries.clear();
    if (d.system == nullptr) return;
    // Todos los cuerpos (tambien los de las mallas estaticas y las ruedas):
    // SetPosition conserva la velocidad y no los despierta.
    JPH::BodyIDVector ids;
    d.system->GetBodies(ids);
    JPH::BodyInterface& bodies = d.bodies();
    const JPH::Vec3 delta = toJolt(offset);
    for (const JPH::BodyID& id : ids) {
        bodies.SetPosition(id, bodies.GetPosition(id) - JPH::RVec3(delta), JPH::EActivation::DontActivate);
    }
}

void PhysicsSystem::clearStaticMeshes() {
    Impl& d = *impl_;
    for (auto& [key, mesh] : d.static_meshes) d.destroyStaticBody(mesh);
    d.static_meshes.clear();
}

std::size_t PhysicsSystem::staticMeshCount() const { return impl_->static_meshes.size(); }

bool PhysicsSystem::running() const {
    return impl_->system != nullptr;
}

int PhysicsSystem::update(ecs::World& world, float delta_seconds, bool simulate) {
    Impl& d = *impl_;
    if (!d.system) return 0;
    d.events.clear();
    const auto begin = std::chrono::steady_clock::now();
    modeling::updateEditableMeshes(world);  // su MeshCollider usa la malla generada
    d.sync(world, simulate);
    d.syncCharacters(world, simulate);
    d.syncRagdolls(world, simulate);
    d.syncCloths(world, simulate);
    d.syncSoftBodies(world, simulate);
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
        d.writeCharacters(world, d.accumulator / step);
        d.writeRagdolls(world, d.accumulator / step);
        d.writeCloths(d.accumulator / step);
        d.writeSoftBodies(world, d.accumulator / step);
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
    d.syncCharacters(world, true);
    d.syncCloths(world, true);
    d.syncSoftBodies(world, true);
    d.stepOnce(world);
    d.writeBack(world, 1.0f);
    d.writeCharacters(world, 1.0f);
    d.writeCloths(1.0f);
    d.writeSoftBodies(world, 1.0f);
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
    if (const auto it = impl_->characters.find(entity.handle()); entity.valid() && it != impl_->characters.end()) {
        return it->second.velocity;
    }
    const Impl::BodyEntry* entry = impl_->entryOf(entity);
    if (entry == nullptr || entry->solid.IsInvalid()) return {};
    return fromJolt(impl_->bodies().GetLinearVelocity(entry->solid));
}

void PhysicsSystem::setVehicleInput(ecs::Entity vehicle, float throttle, float steering, float brake, float handbrake) {
    const auto it = impl_->vehicles.find(vehicle.handle());
    if (it == impl_->vehicles.end()) return;
    it->second.throttle = std::clamp(throttle, -1.0f, 1.0f);
    it->second.steering = std::clamp(steering, -1.0f, 1.0f);
    it->second.brake = std::clamp(brake, 0.0f, 1.0f);
    it->second.handbrake = std::clamp(handbrake, 0.0f, 1.0f);
}

void PhysicsSystem::driveVehiclesWithKeyboard(ecs::World& world, bool forward, bool back, bool left, bool right,
                                              bool handbrake) {
    for (auto& [handle, v] : impl_->vehicles) {
        if (!world.valid(handle)) continue;
        const Vehicle* vehicle = world.wrap(handle).tryGet<Vehicle>();
        if (vehicle == nullptr || !vehicle->keyboard) continue;
        float throttle = (forward ? 1.0f : 0.0f) - (back ? 1.0f : 0.0f);
        float brake = 0.0f;
        // Hacia delante y se pide atras (o al reves): primero frena.
        const ecs::Entity e = world.wrap(handle);
        const float along = core::dot(linearVelocity(e), e.forward());
        if ((throttle < 0.0f && along > 1.0f) || (throttle > 0.0f && along < -1.0f)) {
            brake = 1.0f;
            throttle = 0.0f;
        }
        setVehicleInput(e, throttle, (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f), brake, handbrake ? 1.0f : 0.0f);
    }
}

PhysicsSystem::VehicleState PhysicsSystem::vehicleState(ecs::Entity vehicle) const {
    VehicleState state;
    const auto it = impl_->vehicles.find(vehicle.handle());
    if (it == impl_->vehicles.end() || !it->second.constraint) return state;
    const auto* controller = static_cast<const JPH::WheeledVehicleController*>(it->second.constraint->GetController());
    state.valid = true;
    state.rpm = controller->GetEngine().GetCurrentRPM();
    state.gear = controller->GetTransmission().GetCurrentGear();
    state.speed_kmh = core::length(linearVelocity(vehicle)) * 3.6f;
    for (const JPH::Wheel* wheel : it->second.constraint->GetWheels()) {
        if (wheel->HasContact()) ++state.wheels_on_ground;
    }
    return state;
}

void PhysicsSystem::setLinearVelocity(ecs::Entity entity, const Vec3& velocity) {
    if (isCharacter(entity)) {
        setCharacterVelocity(entity, velocity);
        return;
    }
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

// --- Character Controller --------------------------------------------------------

bool PhysicsSystem::isCharacter(ecs::Entity entity) const {
    const auto it = impl_->characters.find(entity.handle());
    return entity.valid() && it != impl_->characters.end() && it->second.character;
}

PhysicsSystem::CharacterState PhysicsSystem::characterState(ecs::Entity character) const {
    CharacterState state;
    if (!isCharacter(character)) return state;
    const Impl::CharacterEntry& c = impl_->characters.at(character.handle());
    const JPH::CharacterVirtual& ch = *c.character;
    state.valid = true;
    state.ground_state = static_cast<CharacterGround>(static_cast<int>(ch.GetGroundState()));
    state.grounded = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    state.crouching = c.crouching;
    state.velocity = c.velocity;
    if (ch.GetGroundState() != JPH::CharacterBase::EGroundState::InAir) {
        state.ground_normal = fromJolt(ch.GetGroundNormal());
        state.ground_point = fromJolt(JPH::Vec3(ch.GetGroundPosition()));
        state.ground_velocity = fromJolt(ch.GetGroundVelocity());
        state.ground = impl_->entityOfBody(ch.GetGroundBodyID());
    }
    state.collision_flags = c.flags;
    state.jumps_used = c.jumps_used;
    return state;
}

std::uint32_t PhysicsSystem::moveCharacter(ecs::World& world, ecs::Entity character, const Vec3& displacement) {
    Impl& d = *impl_;
    if (!d.system || !character.valid()) return 0;
    const CharacterController* cc = character.tryGet<CharacterController>();
    if (cc == nullptr) return 0;
    d.world = &world;
    if (!isCharacter(character)) {
        // Aun sin capsula (se crea en el siguiente update): solo se desplaza.
        character.setWorldPosition(character.worldPosition() + displacement);
        return 0;
    }
    Impl::CharacterEntry& c = d.characters.at(character.handle());
    // Movido desde fuera desde el ultimo paso: teletransporte antes de moverlo.
    if (const Vec3 now = character.worldPosition(); !nearlyEqual(c.position, now, 1e-4f)) {
        c.character->SetPosition(JPH::RVec3(toJolt(now)));
        c.position = now;
    }
    const float dt = std::max(d.settings.fixed_step, 1e-4f);
    const JPH::Vec3 keep = c.character->GetLinearVelocity();
    d.advanceCharacter(c, character.handle(), *cc, displacement * (1.0f / dt), dt, JPH::Vec3::sZero(), false);
    c.character->SetLinearVelocity(keep);
    // Se ve ya donde acaba (sin interpolar desde donde estaba).
    c.current_position = c.previous_position = fromJolt(JPH::Vec3(c.character->GetPosition()));
    character.setWorldPosition(c.current_position);
    c.position = character.worldPosition();
    return c.flags;
}

void PhysicsSystem::setCharacterInput(ecs::Entity character, const Vec3& direction, bool run) {
    const auto it = impl_->characters.find(character.handle());
    if (it == impl_->characters.end()) return;
    it->second.input = Vec3{direction.x, 0.0f, direction.z};
    it->second.run = run;
}

bool PhysicsSystem::characterJump(ecs::Entity character, float height) {
    const auto it = impl_->characters.find(character.handle());
    if (it == impl_->characters.end() || !it->second.character) return false;
    Impl::CharacterEntry& c = it->second;
    const CharacterController* cc = character.valid() ? character.tryGet<CharacterController>() : nullptr;
    if (cc == nullptr || cc->max_jumps <= 0) return false;
    const bool grounded = c.character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    const bool can = grounded || c.since_grounded <= cc->coyote_time || c.jumps_used < cc->max_jumps;
    // Guardado un momento: si se pulsa justo antes de tocar el suelo, salta al tocarlo.
    c.jump_buffer = 0.15f;
    c.jump_height = height;
    return can;
}

void PhysicsSystem::setCharacterCrouch(ecs::Entity character, bool crouch) {
    const auto it = impl_->characters.find(character.handle());
    if (it != impl_->characters.end()) it->second.crouch_wanted = crouch;
}

void PhysicsSystem::setCharacterVelocity(ecs::Entity character, const Vec3& velocity) {
    const auto it = impl_->characters.find(character.handle());
    if (it == impl_->characters.end() || !it->second.character) return;
    Impl::CharacterEntry& c = it->second;
    c.manual_velocity = velocity;
    c.move_velocity = Vec3{velocity.x, 0.0f, velocity.z};
    const JPH::Vec3 v = c.character->GetLinearVelocity();
    c.character->SetLinearVelocity(JPH::Vec3(v.GetX(), velocity.y, v.GetZ()));
}

void PhysicsSystem::addCharacterVelocity(ecs::Entity character, const Vec3& velocity) {
    const auto it = impl_->characters.find(character.handle());
    if (it != impl_->characters.end()) it->second.pending_velocity = it->second.pending_velocity + velocity;
}

void PhysicsSystem::driveCharactersWithKeyboard(ecs::World& world, const CharacterKeys& keys) {
    if (impl_->characters.empty()) return;
    Vec3 look = keys.camera_forward;
    if (core::length(look) < 1e-6f) {
        // La camara principal (la marcada como principal, o la primera activa).
        ecs::Entity main;
        for (const entt::entity h : world.registry().view<ecs::Camera>()) {
            const ecs::Entity e = world.wrap(h);
            if (!e.activeInHierarchy()) continue;
            const bool is_main = world.registry().get<ecs::Camera>(h).is_main;
            if (!main.valid() || (is_main && !main.get<ecs::Camera>().is_main)) main = e;
        }
        look = main.valid() ? main.forward() : Vec3{0.0f, 0.0f, -1.0f};
    }
    Vec3 forward{look.x, 0.0f, look.z};
    const float len = core::length(forward);
    forward = len > 1e-4f ? forward * (1.0f / len) : Vec3{0.0f, 0.0f, -1.0f};
    const Vec3 right{-forward.z, 0.0f, forward.x};
    Vec3 direction = forward * ((keys.forward ? 1.0f : 0.0f) - (keys.back ? 1.0f : 0.0f)) +
                     right * ((keys.right ? 1.0f : 0.0f) - (keys.left ? 1.0f : 0.0f));
    if (const float l = core::length(direction); l > 1.0f) direction = direction * (1.0f / l);
    for (auto& [handle, c] : impl_->characters) {
        if (!world.valid(handle)) continue;
        const CharacterController* cc = world.registry().try_get<CharacterController>(handle);
        if (cc == nullptr || !cc->keyboard || cc->movement != CharacterMovement::Integrated) continue;
        c.input = direction;
        c.run = keys.run;
        c.crouch_wanted = keys.crouch;
        // El salto cuenta al pulsar (no mientras se mantiene).
        if (keys.jump && !c.jump_held) characterJump(world.wrap(handle));
        c.jump_held = keys.jump;
    }
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
