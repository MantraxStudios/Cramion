// Juntas de fisica para los scripts (Joints.h):
//
//   Physics.createJoint(puerta, "Hinge", marco, {anchor = Vec3(0.5, 0, 0), axis = Vec3(0, 1, 0),
//                       use_limits = true, min_angle = -100, max_angle = 0, break_force = 4000})
//   Physics.breakJoint(puerta, "Hinge")      Physics.repairJoint(puerta)
//   Physics.jointState(puerta, "Hinge")  -> {exists, active, broken, force, torque, angle, position}
//   Physics.removeJoint(puerta, "Hinge")
//
// Tambien se crean con addComponent("HingeJoint") + setField, y se ajustan
// en marcha con setField (motor, muelle, limites).
//
// Evento para los scripts (mensaje, fase Events):
//   OnJointBreak {joint = "HingeJoint", force, torque, connected}

#include "Modules.h"

#include "../ComponentFields.h"

#include "CramionCore/physics/Joints.h"
#include "CramionCore/physics/PhysicsSystem.h"

#include <memory>
#include <string>
#include <vector>

namespace cramion::scripting::native {
namespace {

struct JointsState {
    physics::PhysicsSystem* physics = nullptr;
    int listener = -1;
    std::vector<physics::JointBreakEvent> breaks;
};

// "Hinge", "HingeJoint"... ; nil = todas (-1).
int jointTypeArg(const api::Call& c, std::size_t i, bool allow_all) {
    if (allow_all && !c.has(i)) return -1;
    physics::JointType type{};
    const std::string name = c.string(i);
    if (!physics::jointTypeFromName(name, type)) {
        throw api::Error("junta: Fixed, Hinge, Slider, Distance, Spring o Configurable (no \"" + name + "\")");
    }
    return static_cast<int>(type);
}

// Del script al campo (como setField: bool, numero, texto, Vec3, entidad o nil).
bool toPostValue(const Runtime& rt, const api::Value& value, PostValue& out) {
    switch (value.type()) {
        case api::Value::Type::Bool:
            out.type = PostValue::Type::Bool;
            out.flag = value.truthy();
            return true;
        case api::Value::Type::Number:
            out.type = PostValue::Type::Number;
            out.number = value.asNumber();
            return true;
        case api::Value::Type::String:
            out.type = PostValue::Type::Text;
            out.text = value.asString();
            return true;
        case api::Value::Type::Vec3:
            out.type = PostValue::Type::Vector;
            out.vector = value.asVec3();
            return true;
        case api::Value::Type::Entity: {
            const ecs::Entity x = rt.entity(value.asEntity());
            out.type = PostValue::Type::Text;
            out.text = x.valid() ? x.uuid().toString() : std::string();
            return true;
        }
        default: return false;
    }
}

void dispatchJointBreaks(Runtime& rt, JointsState& state) {
    if (state.breaks.empty()) return;
    std::vector<physics::JointBreakEvent> queue;
    queue.swap(state.breaks);
    if (!rt.message_listener) return;
    for (const physics::JointBreakEvent& event : queue) {
        if (!event.entity.valid()) continue;
        api::Value arg = api::Value::object();
        arg.set("joint", std::string(physics::jointComponentName(event.type)));
        arg.set("force", event.force);
        arg.set("torque", event.torque);
        arg.set("connected", event.connected.valid() ? rt.entityValue(event.connected) : api::Value{});
        rt.message_listener(event.entity, "OnJointBreak", rt.native.toJson(arg).dump());
    }
}

}  // namespace

void registerJointsApi(Runtime& rt) {
    auto state = std::make_shared<JointsState>();
    api::NativeApi& api = rt.native;

    api.function("Physics.createJoint", [&rt](api::Call& c) {
        const ecs::Entity e = rt.entityArg(c, 0);
        const physics::JointType type = static_cast<physics::JointType>(jointTypeArg(c, 1, false));
        if (!e.valid() || e.world() == nullptr) return api::Value(false);
        const ecs::ComponentType* component = ecs::ComponentRegistry::instance().find(physics::jointComponentName(type));
        if (component == nullptr) return api::Value(false);
        ecs::World& world = *e.world();
        if (!component->has(world, e.handle())) component->add(world, e.handle());
        // Con quien (nil = el mundo).
        PostValue connected;
        connected.type = PostValue::Type::Text;
        if (c.has(2)) {
            const ecs::Entity other = rt.entityArg(c, 2);
            connected.text = other.valid() ? other.uuid().toString() : std::string();
        }
        PostFieldVisitor set_connected(PostFieldVisitor::Mode::Set, "connected_body", connected, false);
        component->reflect(world, e.handle(), set_connected);
        // Los demas campos por su clave (los del Inspector: anchor, axis, use_limits...).
        const api::Value& options = c.arg(3);
        if (options.isObject()) {
            for (const auto& [key, value] : options.fields()) {
                PostValue v;
                if (!toPostValue(rt, value, v)) {
                    rt.write(1, "Physics.createJoint: valor no valido en '" + key + "'");
                    continue;
                }
                PostFieldVisitor visitor(PostFieldVisitor::Mode::Set, key, v, false);
                component->reflect(world, e.handle(), visitor);
                if (!visitor.found()) rt.write(1, std::string("Physics.createJoint: ") + physics::jointComponentName(type) +
                                                      " no tiene '" + key + "'");
            }
        }
        return api::Value(true);
    }, {"entidad, \"Hinge\", conectado, {anchor = Vec3(0, 1, 0), axis = Vec3(0, 1, 0), break_force = 500}",
        "Une la entidad a otra (nil = al mundo) con una junta: Fixed, Hinge, Slider, Distance, Spring o Configurable",
        "bool"});
    api.function("Physics.removeJoint", [&rt](api::Call& c) {
        const ecs::Entity e = rt.entityArg(c, 0);
        const physics::JointType type = static_cast<physics::JointType>(jointTypeArg(c, 1, false));
        const ecs::ComponentType* component = ecs::ComponentRegistry::instance().find(physics::jointComponentName(type));
        if (!e.valid() || e.world() == nullptr || component == nullptr || !component->has(*e.world(), e.handle())) {
            return api::Value(false);
        }
        component->remove(*e.world(), e.handle());
        return api::Value(true);
    }, {"entidad, \"Hinge\"", "Quita la junta (deja de unir sin OnJointBreak)", "bool"});
    api.function("Physics.breakJoint", [&rt](api::Call& c) {
        const ecs::Entity e = rt.entityArg(c, 0);
        const int type = jointTypeArg(c, 1, true);
        return api::Value(rt.physics != nullptr && rt.physics->breakJoint(e, type));
    }, {"entidad, \"Hinge\"", "Rompe ya la junta (sin tipo: todas las de la entidad); llega OnJointBreak", "bool"});
    api.function("Physics.repairJoint", [&rt](api::Call& c) {
        const ecs::Entity e = rt.entityArg(c, 0);
        const int type = jointTypeArg(c, 1, true);
        return api::Value(rt.physics != nullptr && rt.physics->repairJoint(e, type));
    }, {"entidad, \"Hinge\"", "Una junta rota vuelve a unir (con su pose de partida)", "bool"});
    api.function("Physics.jointState", [&rt](api::Call& c) {
        const ecs::Entity e = rt.entityArg(c, 0);
        const physics::JointType type = static_cast<physics::JointType>(jointTypeArg(c, 1, false));
        const physics::JointState s = rt.physics != nullptr ? rt.physics->jointState(e, type) : physics::JointState{};
        api::Value out = api::Value::object();
        out.set("exists", s.exists);
        out.set("active", s.active);
        out.set("broken", s.broken);
        out.set("force", s.force);
        out.set("torque", s.torque);
        out.set("angle", s.angle);
        out.set("position", s.position);
        return out;
    }, {"entidad, \"Hinge\"", "{exists, active, broken, force (N), torque (N m), angle (grados), position (m)}", "objeto"});

    // Roturas: el oyente de la fisica vive mientras corre el juego. Se engancha
    // al empezar y, si la fisica llega despues (setPhysics tras start) o
    // cambia, en el siguiente frame.
    const auto attach = [&rt, state] {
        if (rt.physics == nullptr || (state->listener >= 0 && state->physics == rt.physics)) return;
        const std::weak_ptr<JointsState> weak = state;
        state->physics = rt.physics;
        state->listener = rt.physics->addJointBreakListener([weak](const physics::JointBreakEvent& event) {
            if (const auto s = weak.lock()) s->breaks.push_back(event);
        });
    };
    rt.onStart(attach);
    rt.onStop([&rt, state](bool) {
        if (state->listener >= 0 && state->physics != nullptr && state->physics == rt.physics) {
            state->physics->removeJointBreakListener(state->listener);
        }
        state->physics = nullptr;
        state->listener = -1;
        state->breaks.clear();
    });
    rt.onFrame(Phase::Events, [&rt, state, attach](float) {
        attach();
        dispatchJointBreaks(rt, *state);
    });
}

}  // namespace cramion::scripting::native
