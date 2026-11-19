// Juntas de fisica (Joints.h): componentes, su reflexion y las constraints
// de Jolt que las hacen (JointSet.h).

#include "CramionCore/physics/Joints.h"

#include "JointSet.h"

#include "CramionCore/ecs/MathUtil.h"

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace cramion::physics {

using core::Vec3;
using ecs::FloatRange;
using ecs::Vec3Kind;

namespace {

constexpr float kDegToRad = 0.017453292519943295f;
constexpr float kRadToDeg = 57.29577951308232f;

JPH::Vec3 toJ(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
Vec3 fromJ(JPH::Vec3Arg v) { return Vec3{v.GetX(), v.GetY(), v.GetZ()}; }

struct Hasher {
    std::uint64_t h = 1469598103934665603ull;
    void add(std::uint64_t x) {
        h ^= x;
        h *= 1099511628211ull;
    }
    void f(float x) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &x, sizeof(bits));
        add(bits);
    }
    void v(const Vec3& x) {
        f(x.x);
        f(x.y);
        f(x.z);
    }
};

constexpr std::array<const char*, kJointTypeCount> kNames = {"FixedJoint",    "HingeJoint",  "SliderJoint",
                                                              "DistanceJoint", "SpringJoint", "ConfigurableJoint"};

// Lo comun (el Joint de Unity).
void reflectCommon(ecs::PropertyVisitor& v, JointSettings& j, bool has_axis) {
    const bool all = v.wantsAllFields();
    v.entity({"connected_body", "Cuerpo conectado", "Objeto con Rigidbody o collider al que se une.\nVacio = el mundo (un punto fijo)"},
             j.connected_body);
    v.field({"anchor", "Anclaje", "Punto de la junta, en el espacio local del objeto"}, j.anchor, Vec3Kind::Position);
    if (has_axis) {
        v.field({"axis", "Eje", "Eje de giro (bisagra) o de movimiento (corredera), local al objeto"}, j.axis,
                Vec3Kind::Direction);
    }
    v.field({"auto_connected_anchor", "Anclaje conectado automatico",
             "El otro lado es el mismo punto del mundo al empezar (en Distance/Spring, el origen del conectado)"},
            j.auto_connected_anchor);
    if (all || !j.auto_connected_anchor) {
        v.field({"connected_anchor", "  Anclaje conectado", "Local al cuerpo conectado (o del mundo si no hay)"},
                j.connected_anchor, Vec3Kind::Position);
    }
    v.field({"break_force", "Fuerza de rotura", "Newtons que aguanta antes de romperse (0 = no se rompe).\n"
                                                "Al romperse, los scripts reciben OnJointBreak"},
            j.break_force, FloatRange{0.0f, 1.0e9f, 10.0f, "%.0f N"});
    v.field({"break_torque", "Par de rotura", "N m que aguanta antes de romperse (0 = no se rompe)"}, j.break_torque,
            FloatRange{0.0f, 1.0e9f, 10.0f, "%.0f N m"});
    v.field({"enable_collision", "Chocan entre si", "Los dos cuerpos unidos chocan entre ellos (por defecto no)"},
            j.enable_collision);
}

}  // namespace

const char* jointComponentName(JointType type) {
    const int i = static_cast<int>(type);
    return i >= 0 && i < kJointTypeCount ? kNames[static_cast<std::size_t>(i)] : "";
}

bool jointTypeFromName(const std::string& name, JointType& out) {
    std::string key;
    for (const char c : name) key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (key.size() > 5 && key.ends_with("joint")) key.resize(key.size() - 5);
    static const std::pair<const char*, JointType> kKeys[] = {
        {"fixed", JointType::Fixed},     {"fija", JointType::Fixed},          {"hinge", JointType::Hinge},
        {"bisagra", JointType::Hinge},   {"slider", JointType::Slider},       {"corredera", JointType::Slider},
        {"prismatic", JointType::Slider}, {"distance", JointType::Distance},  {"distancia", JointType::Distance},
        {"spring", JointType::Spring},   {"muelle", JointType::Spring},       {"configurable", JointType::Configurable},
        {"6dof", JointType::Configurable}, {"sixdof", JointType::Configurable}};
    for (const auto& [k, t] : kKeys) {
        if (key == k) {
            out = t;
            return true;
        }
    }
    return false;
}

void FixedJoint::reflect(ecs::PropertyVisitor& v) { reflectCommon(v, *this, false); }

void HingeJoint::reflect(ecs::PropertyVisitor& v) {
    const bool all = v.wantsAllFields();
    reflectCommon(v, *this, true);
    v.field({"use_limits", "Limites", "Angulos minimo y maximo desde la pose de partida"}, use_limits);
    if (all || use_limits) {
        v.field({"min_angle", "  Angulo minimo"}, min_angle, FloatRange{-180.0f, 0.0f, 0.5f, "%.1f grados"});
        v.field({"max_angle", "  Angulo maximo"}, max_angle, FloatRange{0.0f, 180.0f, 0.5f, "%.1f grados"});
    }
    v.field({"use_spring", "Muelle", "Tira hacia el angulo objetivo"}, use_spring);
    if (all || use_spring) {
        v.field({"spring", "  Rigidez"}, spring, FloatRange{0.0f, 1.0e7f, 1.0f, "%.1f N m/rad"});
        v.field({"damper", "  Amortiguacion"}, damper, FloatRange{0.0f, 1.0e7f, 0.1f, "%.2f"});
        v.field({"target_angle", "  Angulo objetivo"}, target_angle, FloatRange{-180.0f, 180.0f, 0.5f, "%.1f grados"});
    }
    v.field({"use_motor", "Motor", "Gira a una velocidad con un par maximo (manda sobre el muelle)"}, use_motor);
    if (all || use_motor) {
        v.field({"motor_speed", "  Velocidad"}, motor_speed, FloatRange{-36000.0f, 36000.0f, 1.0f, "%.0f grados/s"});
        v.field({"motor_force", "  Par maximo"}, motor_force, FloatRange{0.0f, 1.0e9f, 1.0f, "%.0f N m"});
    }
    v.field({"friction", "Friccion", "Par que frena el giro sin motor"}, friction, FloatRange{0.0f, 1.0e7f, 0.1f, "%.2f N m"});
}

void SliderJoint::reflect(ecs::PropertyVisitor& v) {
    const bool all = v.wantsAllFields();
    reflectCommon(v, *this, true);
    v.field({"use_limits", "Limites", "Recorrido minimo y maximo desde la pose de partida"}, use_limits);
    if (all || use_limits) {
        v.field({"min_distance", "  Minimo"}, min_distance, FloatRange{-1.0e4f, 0.0f, 0.01f, "%.2f m"});
        v.field({"max_distance", "  Maximo"}, max_distance, FloatRange{0.0f, 1.0e4f, 0.01f, "%.2f m"});
    }
    v.field({"use_spring", "Muelle", "Tira hacia la posicion objetivo"}, use_spring);
    if (all || use_spring) {
        v.field({"spring", "  Rigidez"}, spring, FloatRange{0.0f, 1.0e8f, 1.0f, "%.1f N/m"});
        v.field({"damper", "  Amortiguacion"}, damper, FloatRange{0.0f, 1.0e8f, 0.1f, "%.2f"});
        v.field({"target_position", "  Posicion objetivo"}, target_position, FloatRange{-1.0e4f, 1.0e4f, 0.01f, "%.2f m"});
    }
    v.field({"use_motor", "Motor", "Se mueve a una velocidad con una fuerza maxima (manda sobre el muelle)"}, use_motor);
    if (all || use_motor) {
        v.field({"motor_speed", "  Velocidad"}, motor_speed, FloatRange{-1.0e3f, 1.0e3f, 0.01f, "%.2f m/s"});
        v.field({"motor_force", "  Fuerza maxima"}, motor_force, FloatRange{0.0f, 1.0e9f, 1.0f, "%.0f N"});
    }
    v.field({"friction", "Friccion", "Fuerza que frena el movimiento sin motor"}, friction, FloatRange{0.0f, 1.0e8f, 0.1f, "%.2f N"});
}

void DistanceJoint::reflect(ecs::PropertyVisitor& v) {
    reflectCommon(v, *this, false);
    v.field({"min_distance", "Distancia minima", "-1 = la de partida"}, min_distance, FloatRange{-1.0f, 1.0e4f, 0.01f, "%.2f m"});
    v.field({"max_distance", "Distancia maxima", "-1 = la de partida"}, max_distance, FloatRange{-1.0f, 1.0e4f, 0.01f, "%.2f m"});
    v.field({"spring_frequency", "Muelle (Hz)", "0 = rigida; mas = mas dura"}, spring_frequency,
            FloatRange{0.0f, 100.0f, 0.05f, "%.2f Hz"});
    v.field({"spring_damping", "Amortiguacion", "0..1"}, spring_damping, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
}

void SpringJoint::reflect(ecs::PropertyVisitor& v) { DistanceJoint::reflect(v); }

void ConfigurableJoint::reflect(ecs::PropertyVisitor& v) {
    const bool all = v.wantsAllFields();
    reflectCommon(v, *this, true);
    v.field({"secondary_axis", "Eje secundario", "Y de la junta (se hace perpendicular al eje)"}, secondary_axis,
            Vec3Kind::Direction);
    static constexpr std::array<const char*, 3> kMotion = {"Bloqueado", "Limitado", "Libre"};
    ecs::enumField(v, {"x_motion", "Movimiento X"}, x_motion, kMotion);
    ecs::enumField(v, {"y_motion", "Movimiento Y"}, y_motion, kMotion);
    ecs::enumField(v, {"z_motion", "Movimiento Z"}, z_motion, kMotion);
    ecs::enumField(v, {"angular_x_motion", "Giro X (twist)"}, angular_x_motion, kMotion);
    ecs::enumField(v, {"angular_y_motion", "Giro Y (swing)"}, angular_y_motion, kMotion);
    ecs::enumField(v, {"angular_z_motion", "Giro Z (swing)"}, angular_z_motion, kMotion);
    const bool linear_limited = x_motion == JointMotion::Limited || y_motion == JointMotion::Limited ||
                                z_motion == JointMotion::Limited;
    const bool angular_limited = angular_x_motion == JointMotion::Limited || angular_y_motion == JointMotion::Limited ||
                                 angular_z_motion == JointMotion::Limited;
    if (all || linear_limited) {
        v.field({"linear_limit_min", "Limite lineal minimo", "m (ejes Limitados)"}, linear_limit_min, Vec3Kind::Position);
        v.field({"linear_limit_max", "Limite lineal maximo", "m (ejes Limitados)"}, linear_limit_max, Vec3Kind::Position);
        v.field({"linear_limit_spring", "Limite lineal blando", "Hz (0 = rigido)"}, linear_limit_spring,
                FloatRange{0.0f, 100.0f, 0.05f, "%.2f Hz"});
    }
    if (all || angular_limited) {
        v.field({"angular_limit_min", "Limite de giro minimo", "grados (ejes Limitados)"}, angular_limit_min, Vec3Kind::Euler);
        v.field({"angular_limit_max", "Limite de giro maximo", "grados (ejes Limitados)"}, angular_limit_max, Vec3Kind::Euler);
    }
    v.field({"position_drive", "Empuje a posicion", "Lleva los ejes no bloqueados a la posicion objetivo"}, position_drive);
    if (all || position_drive) {
        v.field({"target_position", "  Posicion objetivo"}, target_position, Vec3Kind::Position);
        v.field({"drive_max_force", "  Fuerza maxima"}, drive_max_force, FloatRange{0.0f, 1.0e9f, 1.0f, "%.0f N"});
    }
    v.field({"rotation_drive", "Empuje a giro", "Lleva los giros no bloqueados al giro objetivo"}, rotation_drive);
    if (all || rotation_drive) {
        v.field({"target_rotation", "  Giro objetivo"}, target_rotation, Vec3Kind::Euler);
        v.field({"drive_max_torque", "  Par maximo"}, drive_max_torque, FloatRange{0.0f, 1.0e9f, 1.0f, "%.0f N m"});
    }
    if (all || position_drive || rotation_drive) {
        v.field({"drive_frequency", "  Frecuencia"}, drive_frequency, FloatRange{0.0f, 100.0f, 0.05f, "%.2f Hz"});
        v.field({"drive_damping", "  Amortiguacion"}, drive_damping, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    }
}

void registerJointComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("HingeJoint") != nullptr) return;
    registry.registerComponent<FixedJoint>("FixedJoint", "Fixed Joint", "Fisica");
    registry.registerComponent<HingeJoint>("HingeJoint", "Hinge Joint (bisagra)", "Fisica");
    registry.registerComponent<SliderJoint>("SliderJoint", "Slider Joint (corredera)", "Fisica");
    registry.registerComponent<DistanceJoint>("DistanceJoint", "Distance Joint", "Fisica");
    registry.registerComponent<SpringJoint>("SpringJoint", "Spring Joint (muelle)", "Fisica");
    registry.registerComponent<ConfigurableJoint>("ConfigurableJoint", "Configurable Joint (6DOF)", "Fisica");
}

// -----------------------------------------------------------------------------
// JointSet
// -----------------------------------------------------------------------------

namespace {

// Recorre las juntas de cada tipo: fn(entidad, tipo, ajustes comunes, puntero al componente).
template <typename Fn>
void forEachJoint(ecs::World& world, Fn&& fn) {
    entt::registry& r = world.registry();
    for (const entt::entity e : r.view<FixedJoint>()) fn(e, JointType::Fixed, static_cast<JointSettings&>(r.get<FixedJoint>(e)));
    for (const entt::entity e : r.view<HingeJoint>()) fn(e, JointType::Hinge, static_cast<JointSettings&>(r.get<HingeJoint>(e)));
    for (const entt::entity e : r.view<SliderJoint>()) fn(e, JointType::Slider, static_cast<JointSettings&>(r.get<SliderJoint>(e)));
    for (const entt::entity e : r.view<DistanceJoint>()) {
        fn(e, JointType::Distance, static_cast<JointSettings&>(r.get<DistanceJoint>(e)));
    }
    for (const entt::entity e : r.view<SpringJoint>()) fn(e, JointType::Spring, static_cast<JointSettings&>(r.get<SpringJoint>(e)));
    for (const entt::entity e : r.view<ConfigurableJoint>()) {
        fn(e, JointType::Configurable, static_cast<JointSettings&>(r.get<ConfigurableJoint>(e)));
    }
}

const JointSettings* settingsOf(const ecs::World& world, entt::entity e, JointType type) {
    const entt::registry& r = const_cast<ecs::World&>(world).registry();
    if (!r.valid(e)) return nullptr;
    switch (type) {
        case JointType::Fixed: return r.try_get<FixedJoint>(e);
        case JointType::Hinge: return r.try_get<HingeJoint>(e);
        case JointType::Slider: return r.try_get<SliderJoint>(e);
        case JointType::Distance: return r.try_get<DistanceJoint>(e);
        case JointType::Spring: return r.try_get<SpringJoint>(e);
        case JointType::Configurable: return r.try_get<ConfigurableJoint>(e);
        case JointType::Count: break;
    }
    return nullptr;
}

JPH::RMat44 bodyTransform(const JPH::BodyInterface& bi, const JPH::BodyID& id) {
    return id.IsInvalid() ? JPH::RMat44::sIdentity() : bi.GetWorldTransform(id);
}

JPH::SixDOFConstraintSettings::EAxis sixAxis(int i) { return static_cast<JPH::SixDOFConstraintSettings::EAxis>(i); }

JointMotion motionOf(const ConfigurableJoint& c, int axis) {
    switch (axis) {
        case 0: return c.x_motion;
        case 1: return c.y_motion;
        case 2: return c.z_motion;
        case 3: return c.angular_x_motion;
        case 4: return c.angular_y_motion;
        default: return c.angular_z_motion;
    }
}

float component(const Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

std::pair<float, float> distanceRange(const DistanceJoint& d, float rest) {
    float lo = d.min_distance < 0.0f ? rest : d.min_distance;
    float hi = d.max_distance < 0.0f ? rest : d.max_distance;
    if (lo > hi) std::swap(lo, hi);
    return {std::max(lo, 0.0f), std::max(hi, 0.0f)};
}

}  // namespace

void JointSet::removeConstraint(JPH::PhysicsSystem& system, Entry& entry) {
    if (entry.constraint) {
        system.RemoveConstraint(entry.constraint.GetPtr());
        // Lo que estaba quieto colgando de ella cae (despierta a los dos).
        JPH::BodyInterface& bi = system.GetBodyInterface();
        if (!entry.body2.IsInvalid() && bi.IsAdded(entry.body2)) bi.ActivateBody(entry.body2);
        if (!entry.body1.IsInvalid() && bi.IsAdded(entry.body1)) bi.ActivateBody(entry.body1);
    }
    entry.constraint = nullptr;
}

void JointSet::setPair(Entry& entry, std::uint64_t pair, JPH::PhysicsSystem& system) {
    if (entry.pair == pair) return;
    if (entry.pair != 0) {
        const auto it = pairs_.find(entry.pair);
        if (it != pairs_.end() && --it->second <= 0) pairs_.erase(it);
    }
    entry.pair = pair;
    if (pair != 0) ++pairs_[pair];
    // Lo que ya se tocaba sigue en la cache de contactos: se vuelve a validar.
    JPH::BodyInterface& bi = system.GetBodyInterface();
    if (!entry.body2.IsInvalid() && bi.IsAdded(entry.body2)) bi.InvalidateContactCache(entry.body2);
}

void JointSet::sync(ecs::World& world, JPH::PhysicsSystem& system, const BodyOf& body_of) {
    ++generation_;
    JPH::BodyInterface& bi = system.GetBodyInterface();
    entt::registry& registry = world.registry();
    forEachJoint(world, [&](entt::entity handle, JointType type, JointSettings& j) {
        const ecs::Entity self = world.wrap(handle);
        if (!self.activeInHierarchy()) return;  // sin seen: se quita abajo
        Entry& entry = joints_[Key{handle, static_cast<int>(type)}];
        entry.seen = generation_;
        entry.break_force = std::max(j.break_force, 0.0f);
        entry.break_torque = std::max(j.break_torque, 0.0f);

        // Los dos cuerpos: el propio y el conectado (o el mundo).
        const JPH::BodyID body2 = body_of(handle);
        entt::entity connected = entt::null;
        JPH::BodyID body1;
        bool connected_ok = true;
        if (j.connected_body.valid()) {
            const ecs::Entity other = world.find(j.connected_body);
            if (!other.valid() || other.handle() == handle || !other.activeInHierarchy()) {
                connected_ok = false;
            } else {
                connected = other.handle();
                body1 = body_of(connected);
                if (body1.IsInvalid()) connected_ok = false;
            }
        }
        const auto dynamic = [&](const JPH::BodyID& id) {
            return !id.IsInvalid() && bi.GetMotionType(id) == JPH::EMotionType::Dynamic;
        };
        const bool usable = !entry.broken && connected_ok && !body2.IsInvalid() && (dynamic(body2) || dynamic(body1));
        if (!usable) {
            removeConstraint(system, entry);
            setPair(entry, 0, system);
            return;
        }

        // Marco de partida (pose de ahora) si es nuevo o cambian anclajes, ejes o con quien.
        Hasher fs;
        fs.add(static_cast<std::uint64_t>(entt::to_integral(connected)));
        fs.v(j.anchor);
        fs.v(j.connected_anchor);
        fs.add(j.auto_connected_anchor ? 1 : 0);
        fs.v(j.axis);
        const ConfigurableJoint* config = type == JointType::Configurable ? registry.try_get<ConfigurableJoint>(handle) : nullptr;
        if (config != nullptr) fs.v(config->secondary_axis);
        if (!entry.framed || entry.frame_sig != fs.h || entry.connected != connected) {
            removeConstraint(system, entry);
            entry.body1 = body1;
            entry.body2 = body2;
            entry.connected = connected;
            const core::Mat4& m = self.worldMatrix();
            Vec3 position{}, scale{};
            core::Quat rotation{};
            ecs::decomposeMatrix(m, position, rotation, scale);
            const Vec3 anchor_world = ecs::transformPoint(m, j.anchor);
            const float axis_length = core::length(j.axis);
            const Vec3 axis_local = axis_length > 1e-6f ? j.axis * (1.0f / axis_length) : Vec3{1.0f, 0.0f, 0.0f};
            const JPH::Vec3 x = toJ(ecs::quatRotate(rotation, axis_local)).NormalizedOr(JPH::Vec3::sAxisX());
            JPH::Vec3 y = x.GetNormalizedPerpendicular();
            if (config != nullptr) {
                JPH::Vec3 s = toJ(ecs::quatRotate(rotation, config->secondary_axis));
                s -= x * x.Dot(s);
                if (s.LengthSq() > 1e-8f) y = s.Normalized();
            }
            Vec3 connected_world = anchor_world;
            if (!j.auto_connected_anchor) {
                connected_world = connected != entt::null ? ecs::transformPoint(world.wrap(connected).worldMatrix(), j.connected_anchor)
                                                          : j.connected_anchor;
            } else if ((type == JointType::Distance || type == JointType::Spring) && connected != entt::null) {
                connected_world = world.wrap(connected).worldPosition();
            }
            const auto toLocal = [&](const JPH::BodyID& id, const Vec3& point, Frame& frame) {
                const JPH::RMat44 inverse = bodyTransform(bi, id).InversedRotationTranslation();
                frame.point = JPH::Vec3(inverse * JPH::RVec3(toJ(point)));
                frame.axis_x = inverse.Multiply3x3(x).NormalizedOr(JPH::Vec3::sAxisX());
                frame.axis_y = inverse.Multiply3x3(y).NormalizedOr(JPH::Vec3::sAxisY());
            };
            toLocal(body1, connected_world, entry.frame1);
            toLocal(body2, anchor_world, entry.frame2);
            entry.rest_distance = core::length(connected_world - anchor_world);
            entry.framed = true;
            entry.frame_sig = fs.h;
            entry.build_sig = 0;
        }

        // Lo que obliga a rehacer la constraint (el resto se cambia en marcha).
        Hasher bs;
        bs.add(body1.GetIndexAndSequenceNumber());
        bs.add(body2.GetIndexAndSequenceNumber());
        bs.add(static_cast<std::uint64_t>(type));
        if (config != nullptr) {
            for (int i = 0; i < 6; ++i) bs.add(static_cast<std::uint64_t>(motionOf(*config, i)));
            bs.v(config->linear_limit_min);
            bs.v(config->linear_limit_max);
            bs.v(config->angular_limit_min);
            bs.v(config->angular_limit_max);
            bs.f(config->linear_limit_spring);
        }
        if (!entry.constraint || entry.build_sig != bs.h || entry.body1 != body1 || entry.body2 != body2) {
            removeConstraint(system, entry);
            entry.body1 = body1;
            entry.body2 = body2;
            const JPH::RMat44 t1 = bodyTransform(bi, body1);
            const JPH::RMat44 t2 = bodyTransform(bi, body2);
            const JPH::RVec3 p1 = t1 * JPH::RVec3(entry.frame1.point);
            const JPH::RVec3 p2 = t2 * JPH::RVec3(entry.frame2.point);
            const JPH::Vec3 x1 = t1.Multiply3x3(entry.frame1.axis_x), y1 = t1.Multiply3x3(entry.frame1.axis_y);
            const JPH::Vec3 x2 = t2.Multiply3x3(entry.frame2.axis_x), y2 = t2.Multiply3x3(entry.frame2.axis_y);
            JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
            switch (type) {
                case JointType::Fixed: {
                    auto* s = new JPH::FixedConstraintSettings;
                    s->mSpace = JPH::EConstraintSpace::WorldSpace;
                    s->mAutoDetectPoint = false;
                    s->mPoint1 = p1;
                    s->mAxisX1 = x1;
                    s->mAxisY1 = y1;
                    s->mPoint2 = p2;
                    s->mAxisX2 = x2;
                    s->mAxisY2 = y2;
                    settings = s;
                    break;
                }
                case JointType::Hinge: {
                    auto* s = new JPH::HingeConstraintSettings;
                    s->mSpace = JPH::EConstraintSpace::WorldSpace;
                    s->mPoint1 = p1;
                    s->mHingeAxis1 = x1;
                    s->mNormalAxis1 = y1;
                    s->mPoint2 = p2;
                    s->mHingeAxis2 = x2;
                    s->mNormalAxis2 = y2;
                    settings = s;
                    break;
                }
                case JointType::Slider: {
                    auto* s = new JPH::SliderConstraintSettings;
                    s->mSpace = JPH::EConstraintSpace::WorldSpace;
                    s->mAutoDetectPoint = false;
                    s->mPoint1 = p1;
                    s->mSliderAxis1 = x1;
                    s->mNormalAxis1 = y1;
                    s->mPoint2 = p2;
                    s->mSliderAxis2 = x2;
                    s->mNormalAxis2 = y2;
                    settings = s;
                    break;
                }
                case JointType::Distance:
                case JointType::Spring: {
                    auto* s = new JPH::DistanceConstraintSettings;
                    s->mSpace = JPH::EConstraintSpace::WorldSpace;
                    s->mPoint1 = p1;
                    s->mPoint2 = p2;
                    const auto range = distanceRange(static_cast<const DistanceJoint&>(j), entry.rest_distance);
                    s->mMinDistance = range.first;
                    s->mMaxDistance = range.second;
                    settings = s;
                    break;
                }
                case JointType::Configurable: {
                    auto* s = new JPH::SixDOFConstraintSettings;
                    s->mSpace = JPH::EConstraintSpace::WorldSpace;
                    s->mPosition1 = p1;
                    s->mAxisX1 = x1;
                    s->mAxisY1 = y1;
                    s->mPosition2 = p2;
                    s->mAxisX2 = x2;
                    s->mAxisY2 = y2;
                    s->mSwingType = JPH::ESwingType::Pyramid;  // admite limites asimetricos
                    for (int i = 0; i < 6; ++i) {
                        const JointMotion motion = motionOf(*config, i);
                        if (motion == JointMotion::Locked) {
                            s->MakeFixedAxis(sixAxis(i));
                        } else if (motion == JointMotion::Free) {
                            s->MakeFreeAxis(sixAxis(i));
                        } else if (i < 3) {
                            const float lo = std::min(component(config->linear_limit_min, i), component(config->linear_limit_max, i));
                            const float hi = std::max(component(config->linear_limit_min, i), component(config->linear_limit_max, i));
                            s->SetLimitedAxis(sixAxis(i), lo, hi);
                        } else {
                            const float a = std::clamp(component(config->angular_limit_min, i - 3), -179.0f, 179.0f) * kDegToRad;
                            const float b = std::clamp(component(config->angular_limit_max, i - 3), -179.0f, 179.0f) * kDegToRad;
                            s->SetLimitedAxis(sixAxis(i), std::min(a, b), std::max(a, b));
                        }
                    }
                    if (config->linear_limit_spring > 0.0f) {
                        for (int i = 0; i < 3; ++i) {
                            s->mLimitsSpringSettings[i] =
                                JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, config->linear_limit_spring, 0.5f);
                        }
                    }
                    settings = s;
                    break;
                }
                case JointType::Count: break;
            }
            if (!settings) return;
            JPH::TwoBodyConstraint* c = bi.CreateConstraint(settings.GetPtr(), body1, body2);
            if (c == nullptr) return;
            entry.constraint = c;
            system.AddConstraint(c);
            bi.ActivateConstraint(c);
            entry.build_sig = bs.h;
            entry.live_sig = 0;
        }

        // Ajustes en marcha: limites, muelles, motores y friccion.
        Hasher ls;
        ls.add(static_cast<std::uint64_t>(type));
        JPH::TwoBodyConstraint* c = entry.constraint.GetPtr();
        switch (type) {
            case JointType::Hinge: {
                const HingeJoint& h = registry.get<HingeJoint>(handle);
                for (const float f : {h.use_limits ? 1.0f : 0.0f, h.min_angle, h.max_angle, h.use_spring ? 1.0f : 0.0f, h.spring,
                                      h.damper, h.target_angle, h.use_motor ? 1.0f : 0.0f, h.motor_speed, h.motor_force,
                                      h.friction}) {
                    ls.f(f);
                }
                if (ls.h == entry.live_sig) break;
                auto* hc = static_cast<JPH::HingeConstraint*>(c);
                if (h.use_limits) {
                    hc->SetLimits(std::clamp(h.min_angle, -180.0f, 0.0f) * kDegToRad, std::clamp(h.max_angle, 0.0f, 180.0f) * kDegToRad);
                } else {
                    hc->SetLimits(-JPH::JPH_PI, JPH::JPH_PI);
                }
                hc->SetMaxFrictionTorque(std::max(h.friction, 0.0f));
                JPH::MotorSettings& ms = hc->GetMotorSettings();
                if (h.use_motor) {
                    ms.SetTorqueLimit(std::max(h.motor_force, 0.0f));
                    hc->SetMotorState(JPH::EMotorState::Velocity);
                    hc->SetTargetAngularVelocity(h.motor_speed * kDegToRad);
                } else if (h.use_spring && h.spring > 0.0f) {
                    ms.mSpringSettings = JPH::SpringSettings(JPH::ESpringMode::StiffnessAndDamping, h.spring, std::max(h.damper, 0.0f));
                    ms.SetTorqueLimit(FLT_MAX);
                    hc->SetMotorState(JPH::EMotorState::Position);
                    hc->SetTargetAngle(h.target_angle * kDegToRad);
                } else {
                    hc->SetMotorState(JPH::EMotorState::Off);
                }
                break;
            }
            case JointType::Slider: {
                const SliderJoint& sj = registry.get<SliderJoint>(handle);
                for (const float f : {sj.use_limits ? 1.0f : 0.0f, sj.min_distance, sj.max_distance, sj.use_spring ? 1.0f : 0.0f,
                                      sj.spring, sj.damper, sj.target_position, sj.use_motor ? 1.0f : 0.0f, sj.motor_speed,
                                      sj.motor_force, sj.friction}) {
                    ls.f(f);
                }
                if (ls.h == entry.live_sig) break;
                auto* sc = static_cast<JPH::SliderConstraint*>(c);
                if (sj.use_limits) {
                    sc->SetLimits(std::min(sj.min_distance, 0.0f), std::max(sj.max_distance, 0.0f));
                } else {
                    sc->SetLimits(-FLT_MAX, FLT_MAX);
                }
                sc->SetMaxFrictionForce(std::max(sj.friction, 0.0f));
                JPH::MotorSettings& ms = sc->GetMotorSettings();
                if (sj.use_motor) {
                    ms.SetForceLimit(std::max(sj.motor_force, 0.0f));
                    sc->SetMotorState(JPH::EMotorState::Velocity);
                    sc->SetTargetVelocity(sj.motor_speed);
                } else if (sj.use_spring && sj.spring > 0.0f) {
                    ms.mSpringSettings = JPH::SpringSettings(JPH::ESpringMode::StiffnessAndDamping, sj.spring, std::max(sj.damper, 0.0f));
                    ms.SetForceLimit(FLT_MAX);
                    sc->SetMotorState(JPH::EMotorState::Position);
                    sc->SetTargetPosition(sj.target_position);
                } else {
                    sc->SetMotorState(JPH::EMotorState::Off);
                }
                break;
            }
            case JointType::Distance:
            case JointType::Spring: {
                const DistanceJoint& d = static_cast<const DistanceJoint&>(j);
                for (const float f : {d.min_distance, d.max_distance, d.spring_frequency, d.spring_damping}) ls.f(f);
                if (ls.h == entry.live_sig) break;
                auto* dc = static_cast<JPH::DistanceConstraint*>(c);
                const auto range = distanceRange(d, entry.rest_distance);
                dc->SetDistance(range.first, range.second);
                dc->SetLimitsSpringSettings(JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping,
                                                                std::max(d.spring_frequency, 0.0f),
                                                                std::clamp(d.spring_damping, 0.0f, 1.0f)));
                break;
            }
            case JointType::Configurable: {
                for (const float f : {config->position_drive ? 1.0f : 0.0f, config->rotation_drive ? 1.0f : 0.0f,
                                      config->drive_frequency, config->drive_damping, config->drive_max_force,
                                      config->drive_max_torque}) {
                    ls.f(f);
                }
                ls.v(config->target_position);
                ls.v(config->target_rotation);
                if (ls.h == entry.live_sig) break;
                auto* sc = static_cast<JPH::SixDOFConstraint*>(c);
                const JPH::SpringSettings spring(JPH::ESpringMode::FrequencyAndDamping, std::max(config->drive_frequency, 0.0f),
                                                 std::clamp(config->drive_damping, 0.0f, 1.0f));
                for (int i = 0; i < 6; ++i) {
                    const bool linear = i < 3;
                    const bool on = (linear ? config->position_drive : config->rotation_drive) &&
                                    motionOf(*config, i) != JointMotion::Locked && config->drive_frequency > 0.0f;
                    JPH::MotorSettings& ms = sc->GetMotorSettings(sixAxis(i));
                    ms.mSpringSettings = spring;
                    if (linear) ms.SetForceLimit(std::max(config->drive_max_force, 0.0f));
                    else ms.SetTorqueLimit(std::max(config->drive_max_torque, 0.0f));
                    sc->SetMotorState(sixAxis(i), on ? JPH::EMotorState::Position : JPH::EMotorState::Off);
                }
                sc->SetTargetPositionCS(toJ(config->target_position));
                const core::Quat q = ecs::quatFromEulerDegrees(config->target_rotation);
                sc->SetTargetOrientationCS(JPH::Quat(q.x, q.y, q.z, q.w).Normalized());
                break;
            }
            case JointType::Fixed:
            case JointType::Count: break;
        }
        if (ls.h != entry.live_sig) {
            entry.live_sig = ls.h;
            bi.ActivateConstraint(c);  // un motor nuevo en un cuerpo dormido
        }

        // Sin colision entre los dos (como Unity), salvo enable_collision.
        const std::uint64_t pair = (!j.enable_collision && connected != entt::null)
                                       ? pairKey(static_cast<std::uint64_t>(entt::to_integral(handle)),
                                                 static_cast<std::uint64_t>(entt::to_integral(connected)))
                                       : 0;
        setPair(entry, pair, system);
    });
    // Las que ya no estan (componente quitado, entidad borrada o inactiva).
    for (auto it = joints_.begin(); it != joints_.end();) {
        if (it->second.seen != generation_) {
            removeConstraint(system, it->second);
            setPair(it->second, 0, system);
            it = joints_.erase(it);
        } else {
            ++it;
        }
    }
}

void JointSet::afterStep(ecs::World* world, JPH::PhysicsSystem& system, float dt) {
    const float inv_dt = dt > 1e-6f ? 1.0f / dt : 0.0f;
    for (auto& [key, entry] : joints_) {
        if (!entry.constraint) continue;
        const JPH::TwoBodyConstraint* c = entry.constraint.GetPtr();
        float linear = 0.0f;
        float angular = 0.0f;
        switch (static_cast<JointType>(key.second)) {
            case JointType::Fixed: {
                const auto* f = static_cast<const JPH::FixedConstraint*>(c);
                linear = f->GetTotalLambdaPosition().Length();
                angular = f->GetTotalLambdaRotation().Length();
                break;
            }
            case JointType::Hinge: {
                const auto* h = static_cast<const JPH::HingeConstraint*>(c);
                const JPH::Vector<2> r = h->GetTotalLambdaRotation();
                linear = h->GetTotalLambdaPosition().Length();
                angular = std::sqrt(r[0] * r[0] + r[1] * r[1]) + std::abs(h->GetTotalLambdaRotationLimits());
                break;
            }
            case JointType::Slider: {
                const auto* s = static_cast<const JPH::SliderConstraint*>(c);
                const JPH::Vector<2> p = s->GetTotalLambdaPosition();
                linear = std::sqrt(p[0] * p[0] + p[1] * p[1]) + std::abs(s->GetTotalLambdaPositionLimits());
                angular = s->GetTotalLambdaRotation().Length();
                break;
            }
            case JointType::Distance:
            case JointType::Spring:
                linear = std::abs(static_cast<const JPH::DistanceConstraint*>(c)->GetTotalLambdaPosition());
                break;
            case JointType::Configurable: {
                const auto* s = static_cast<const JPH::SixDOFConstraint*>(c);
                linear = s->GetTotalLambdaPosition().Length();
                angular = s->GetTotalLambdaRotation().Length();
                break;
            }
            case JointType::Count: break;
        }
        entry.force = linear * inv_dt;
        entry.torque = angular * inv_dt;
        const bool snap = (entry.break_force > 0.0f && entry.force > entry.break_force) ||
                          (entry.break_torque > 0.0f && entry.torque > entry.break_torque);
        if (!snap) continue;
        entry.broken = true;
        removeConstraint(system, entry);
        setPair(entry, 0, system);
        if (world != nullptr && world->valid(key.first)) {
            JointBreakEvent event;
            event.entity = world->wrap(key.first);
            event.type = static_cast<JointType>(key.second);
            event.connected = entry.connected != entt::null && world->valid(entry.connected) ? world->wrap(entry.connected) : ecs::Entity{};
            event.force = entry.force;
            event.torque = entry.torque;
            breaks_.push_back(event);
        }
    }
}

void JointSet::removeBody(JPH::PhysicsSystem& system, const JPH::BodyID& id) {
    if (id.IsInvalid()) return;
    for (auto& [key, entry] : joints_) {
        if (entry.constraint && (entry.body1 == id || entry.body2 == id)) removeConstraint(system, entry);
    }
}

void JointSet::shiftOrigin(JPH::PhysicsSystem& system, JPH::Vec3Arg offset) {
    for (auto& [key, entry] : joints_) {
        if (!entry.framed || entry.connected != entt::null) continue;
        entry.frame1.point -= offset;
        if (entry.constraint) {
            // Sin despertar a nadie: se rehace igual en el siguiente sync.
            system.RemoveConstraint(entry.constraint.GetPtr());
            entry.constraint = nullptr;
        }
        entry.build_sig = 0;
    }
}

void JointSet::clear(JPH::PhysicsSystem* system) {
    if (system != nullptr) {
        for (auto& [key, entry] : joints_) {
            if (entry.constraint) system->RemoveConstraint(entry.constraint.GetPtr());
            entry.constraint = nullptr;
        }
    }
    joints_.clear();
    pairs_.clear();
    breaks_.clear();
}

std::vector<JointBreakEvent> JointSet::takeBreaks() {
    std::vector<JointBreakEvent> out;
    out.swap(breaks_);
    return out;
}

bool JointSet::breakJoint(ecs::World* world, JPH::PhysicsSystem& system, entt::entity entity, int type, bool notify) {
    bool any = false;
    for (auto& [key, entry] : joints_) {
        if (key.first != entity || (type >= 0 && key.second != type) || entry.broken) continue;
        entry.broken = true;
        removeConstraint(system, entry);
        setPair(entry, 0, system);
        any = true;
        if (notify && world != nullptr && world->valid(entity)) {
            JointBreakEvent event;
            event.entity = world->wrap(entity);
            event.type = static_cast<JointType>(key.second);
            event.connected = entry.connected != entt::null && world->valid(entry.connected) ? world->wrap(entry.connected) : ecs::Entity{};
            event.force = entry.force;
            event.torque = entry.torque;
            breaks_.push_back(event);
        }
    }
    return any;
}

bool JointSet::repair(entt::entity entity, int type) {
    bool any = false;
    for (auto& [key, entry] : joints_) {
        if (key.first != entity || (type >= 0 && key.second != type) || !entry.broken) continue;
        entry.broken = false;
        entry.build_sig = 0;
        any = true;
    }
    return any;
}

JointState JointSet::state(const ecs::World& world, entt::entity entity, JointType type, const JPH::PhysicsSystem* system) const {
    JointState s;
    s.exists = settingsOf(world, entity, type) != nullptr;
    const auto it = joints_.find(Key{entity, static_cast<int>(type)});
    if (it == joints_.end()) return s;
    const Entry& e = it->second;
    s.active = e.constraint != nullptr;
    s.broken = e.broken;
    s.force = e.force;
    s.torque = e.torque;
    if (!e.constraint) return s;
    if (type == JointType::Hinge) {
        s.angle = static_cast<const JPH::HingeConstraint*>(e.constraint.GetPtr())->GetCurrentAngle() * kRadToDeg;
    } else if (type == JointType::Slider) {
        s.position = static_cast<const JPH::SliderConstraint*>(e.constraint.GetPtr())->GetCurrentPosition();
    } else if ((type == JointType::Distance || type == JointType::Spring) && system != nullptr) {
        const JPH::BodyInterface& bi = system->GetBodyInterface();
        const JPH::RVec3 a = bodyTransform(bi, e.body1) * JPH::RVec3(e.frame1.point);
        const JPH::RVec3 b = bodyTransform(bi, e.body2) * JPH::RVec3(e.frame2.point);
        s.position = static_cast<float>((a - b).Length());
    }
    return s;
}

std::vector<JointDebug> JointSet::debug(ecs::World& world, const JPH::PhysicsSystem* system) const {
    std::vector<JointDebug> out;
    forEachJoint(world, [&](entt::entity handle, JointType type, JointSettings& j) {
        const ecs::Entity self = world.wrap(handle);
        if (!self.activeInHierarchy()) return;
        JointDebug d;
        d.entity = self;
        d.type = type;
        const auto it = joints_.find(Key{handle, static_cast<int>(type)});
        if (it != joints_.end() && it->second.framed && system != nullptr && it->second.constraint) {
            const Entry& e = it->second;
            const JPH::BodyInterface& bi = system->GetBodyInterface();
            const JPH::RMat44 t1 = bodyTransform(bi, e.body1);
            const JPH::RMat44 t2 = bodyTransform(bi, e.body2);
            d.connected_anchor = fromJ(JPH::Vec3(t1 * JPH::RVec3(e.frame1.point)));
            d.anchor = fromJ(JPH::Vec3(t2 * JPH::RVec3(e.frame2.point)));
            d.axis = fromJ(t2.Multiply3x3(e.frame2.axis_x));
            d.active = true;
        } else {
            // Sin simular (edicion): desde el componente.
            const core::Mat4& m = self.worldMatrix();
            d.anchor = ecs::transformPoint(m, j.anchor);
            d.axis = ecs::transformDirection(m, j.axis);
            const float l = core::length(d.axis);
            d.axis = l > 1e-6f ? d.axis * (1.0f / l) : Vec3{1.0f, 0.0f, 0.0f};
            d.connected_anchor = d.anchor;
            if (j.connected_body.valid()) {
                const ecs::Entity other = world.find(j.connected_body);
                if (other.valid()) {
                    d.connected_anchor = j.auto_connected_anchor
                                             ? ((type == JointType::Distance || type == JointType::Spring) ? other.worldPosition() : d.anchor)
                                             : ecs::transformPoint(other.worldMatrix(), j.connected_anchor);
                }
            } else if (!j.auto_connected_anchor) {
                d.connected_anchor = j.connected_anchor;
            }
        }
        d.broken = it != joints_.end() && it->second.broken;
        out.push_back(d);
    });
    return out;
}

std::size_t JointSet::activeCount() const {
    std::size_t n = 0;
    for (const auto& [key, entry] : joints_) n += entry.constraint ? 1 : 0;
    return n;
}

}  // namespace cramion::physics
