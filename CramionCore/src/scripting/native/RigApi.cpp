// Huesos, IK, ragdoll y sockets de las entidades (Entity:getBonePosition,
// Entity:setIKTarget, Entity.ragdoll, Entity:attachToBone...), su navegacion
// (Entity:moveTo, Entity.isMoving...) y la tabla Navigation, y el Character
// Controller (tabla CharacterController y Entity:move, isGrounded, jump...).
//
// La pose de los huesos y el esqueleto de un modelo los da el programa
// (rt.skeleton_host); sin el, las funciones de huesos devuelven nil.

#include "Modules.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/Rigging.h"
#include "CramionCore/navigation/Navigation.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/physics/Ragdoll.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace cramion::scripting::native {
namespace {

using core::Vec3;

// --- Huesos ---------------------------------------------------------------------------

// Mover huesos encima de la animacion (componente Skeleton): el cambio de
// ese hueso (se crea si no estaba).
ecs::BoneOverride& boneOverride(ecs::Entity x, const std::string& bone) {
    ecs::Skeleton& sk = x.has<ecs::Skeleton>() ? x.get<ecs::Skeleton>() : x.add<ecs::Skeleton>();
    for (ecs::BoneOverride& o : sk.bones) {
        if (o.bone == bone) return o;
    }
    ecs::BoneOverride o;
    o.bone = bone;
    sk.bones.push_back(o);
    return sk.bones.back();
}

// Esqueleto del modelo de la entidad (nullptr si el programa no lo da).
const asset::ModelData* skeletonOf(const Runtime& rt, ecs::Entity x, float* scale) {
    return rt.skeleton_host.skeleton ? rt.skeleton_host.skeleton(x, scale) : nullptr;
}

void registerBones(Runtime& rt) {
    api::NativeApi& api = rt.native;
    api.method("Entity", "getBones", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        api::Value::Array names;
        if (rt.skeleton_host.bone_names) {
            for (const std::string& name : rt.skeleton_host.bone_names(x)) names.emplace_back(name);
        }
        return api::Value(std::move(names));
    }, {"", "Nombres de los huesos", "lista de texto"});
    api.method("Entity", "getBonePosition", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const std::string bone = c.string(0);
        core::Mat4 m;
        if (!rt.skeleton_host.bone_world || !rt.skeleton_host.bone_world(x, bone, m)) return api::Value{};
        return api::Value(Vec3{m.m[3][0], m.m[3][1], m.m[3][2]});
    }, {"\"Head\"", "Posicion del hueso en el mundo", "Vec3 o nil"});
    api.method("Entity", "getBoneRotation", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const std::string bone = c.string(0);
        core::Mat4 m;
        if (!rt.skeleton_host.bone_world || !rt.skeleton_host.bone_world(x, bone, m)) return api::Value{};
        Vec3 t{};
        core::Quat r{};
        Vec3 sc{};
        ecs::decomposeMatrix(m, t, r, sc);
        return api::Value(core::normalize(r));
    }, {"\"Head\"", "Giro del hueso en el mundo", "Quat o nil"});
    // Giro en grados (Vec3) o un Quat, y el peso (0..1) encima de la animacion.
    api.method("Entity", "setBoneRotation", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        ecs::BoneOverride& o = boneOverride(x, c.string(0));
        const api::Value& rotation = c.arg(1);
        if (rotation.isQuat()) o.rotation = ecs::quatToEulerDegrees(rotation.asQuat());
        else if (rotation.isVec3()) o.rotation = rotation.asVec3();
        if (c.has(2)) o.weight = static_cast<float>(c.number(2));
        return api::Value{};
    }, {"\"Head\", Vec3 grados, peso", "Gira el hueso encima de la animacion"});
    api.method("Entity", "setBoneOffset", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const std::string bone = c.string(0);
        const Vec3 offset = c.vec3(1);
        boneOverride(x, bone).position = offset;
        return api::Value{};
    }, {"\"Hips\", Vec3", "Desplaza el hueso"});
    // Escala: un Vec3 o un numero (igual en los tres ejes).
    api.method("Entity", "setBoneScale", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const std::string bone = c.string(0);
        const api::Value& scale = c.arg(1);
        if (scale.isVec3()) {
            boneOverride(x, bone).scale = scale.asVec3();
        } else if (scale.isNumber()) {
            const float s = static_cast<float>(scale.asNumber());
            boneOverride(x, bone).scale = Vec3{s, s, s};
        }
        return api::Value{};
    }, {"\"Head\", 1.2", "Escala el hueso"});
    api.method("Entity", "resetBone", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const std::string bone = c.string(0);
        if (ecs::Skeleton* sk = x.tryGet<ecs::Skeleton>()) {
            sk->bones.erase(std::remove_if(sk->bones.begin(), sk->bones.end(),
                                           [&](const ecs::BoneOverride& o) { return o.bone == bone; }),
                            sk->bones.end());
        }
        return api::Value{};
    }, {"\"Head\"", "Quita los cambios del hueso"});
    api.method("Entity", "resetBones", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        if (ecs::Skeleton* sk = x.tryGet<ecs::Skeleton>()) sk->bones.clear();
        return api::Value{};
    }, {"", "Quita los cambios de todos los huesos"});
    api.method("Entity", "showBones", [&rt](api::Call& c) {
        ecs::Entity x = rt.selfEntity(c);
        ecs::Skeleton& sk = x.has<ecs::Skeleton>() ? x.get<ecs::Skeleton>() : x.add<ecs::Skeleton>();
        sk.show_bones = c.boolean(0, true);
        return api::Value{};
    }, {"true", "Dibuja los huesos en la Escena"});
}

// --- IK -------------------------------------------------------------------------------

ecs::InverseKinematics& ikOf(ecs::Entity x) {
    return x.has<ecs::InverseKinematics>() ? x.get<ecs::InverseKinematics>() : x.add<ecs::InverseKinematics>();
}

// Las cuatro extremidades del humanoide por su nombre (nullptr si es otra cosa).
ecs::IKLimb* limbOf(ecs::InverseKinematics& ik, const std::string& name) {
    if (name == "left_hand" || name == "LeftHand") return &ik.left_hand;
    if (name == "right_hand" || name == "RightHand") return &ik.right_hand;
    if (name == "left_foot" || name == "LeftFoot") return &ik.left_foot;
    if (name == "right_foot" || name == "RightFoot") return &ik.right_foot;
    return nullptr;
}

// La cadena que acaba en `bone` (se crea si `create`).
ecs::IKChain* chainOf(ecs::InverseKinematics& ik, const std::string& bone, bool create) {
    for (ecs::IKChain& chain : ik.chains) {
        if (chain.bone == bone) return &chain;
    }
    if (!create) return nullptr;
    ecs::IKChain chain;
    chain.bone = bone;
    ik.chains.push_back(chain);
    return &ik.chains.back();
}

// Objetivo del argumento i: una entidad, un punto (Vec3) o nil (quitarlo).
void assignTarget(const Runtime& rt, const api::Call& c, std::size_t i, Uuid& id, bool& use_position, Vec3& position) {
    const api::Value& target = c.arg(i);
    if (target.isEntity()) {
        const ecs::Entity t = rt.entityArg(c, i);
        id = t.valid() ? t.uuid() : Uuid{};
        use_position = false;
    } else if (target.isVec3()) {
        position = target.asVec3();
        use_position = true;
    } else {
        id = Uuid{};
        use_position = false;
    }
}

void registerIK(Runtime& rt) {
    api::NativeApi& api = rt.native;
    // e:setIKTarget("left_hand", objetivo) o e:setIKTarget("Pata_D_Delante", objetivo, 3)
    api.method("Entity", "setIKTarget", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const std::string name = c.string(0);
        ecs::InverseKinematics& ik = ikOf(x);
        if (ecs::IKLimb* limb = limbOf(ik, name)) {
            assignTarget(rt, c, 1, limb->target, limb->use_position, limb->position);
            return api::Value{};
        }
        ecs::IKChain* chain = chainOf(ik, name, true);
        assignTarget(rt, c, 1, chain->target, chain->use_position, chain->position);
        chain->ground = false;
        if (c.has(2)) chain->length = static_cast<int>(std::clamp<long long>(c.integer(2), 1, 16));
        return api::Value{};
    }, {"\"PieIzq\", objetivo, huesos", "Objetivo de una cadena IK (entidad, Vec3 o nil)"});
    api.method("Entity", "setIKHint", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const std::string name = c.string(0);
        const ecs::Entity h = c.arg(1).isEntity() ? rt.entityArg(c, 1) : ecs::Entity{};
        const Uuid id = h.valid() ? h.uuid() : Uuid{};
        ecs::InverseKinematics& ik = ikOf(x);
        if (ecs::IKLimb* limb = limbOf(ik, name)) limb->hint = id;
        else if (ecs::IKChain* chain = chainOf(ik, name, false)) chain->hint = id;
        return api::Value{};
    }, {"\"PieIzq\", objetivo", "Pole (hacia donde dobla)"});
    // Tambien "look" (la mirada) y "feet" / "ground" (pies al suelo).
    api.method("Entity", "setIKWeight", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const std::string name = c.string(0);
        const float weight = std::clamp(static_cast<float>(c.number(1)), 0.0f, 1.0f);
        ecs::InverseKinematics& ik = ikOf(x);
        if (ecs::IKLimb* limb = limbOf(ik, name)) limb->weight = weight;
        else if (name == "look") ik.look_weight = weight;
        else if (name == "feet" || name == "ground") ik.grounding_weight = weight;
        else if (ecs::IKChain* chain = chainOf(ik, name, false)) chain->weight = weight;
        return api::Value{};
    }, {"\"PieIzq\", 1", "Peso 0..1"});
    api.method("Entity", "setLookAt", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        ecs::InverseKinematics& ik = ikOf(x);
        assignTarget(rt, c, 0, ik.look_at, ik.look_use_position, ik.look_position);
        if (c.has(1)) ik.look_weight = std::clamp(static_cast<float>(c.number(1)), 0.0f, 1.0f);
        return api::Value{};
    }, {"objetivo, peso", "Mirar con cabeza y cuello"});
    api.method("Entity", "setFootGrounding", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const bool on = c.boolean(0);
        ecs::InverseKinematics& ik = ikOf(x);
        ik.foot_grounding = on;
        for (ecs::IKChain& chain : ik.chains) {
            if (chain.ground) chain.weight = on ? std::max(chain.weight, 1.0f) : 0.0f;
        }
        return api::Value{};
    }, {"true", "Patas al suelo"});

    // Configuracion automatica desde el esqueleto.
    api.method("Entity", "setupCreatureIK", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        float scale = 1.0f;
        const asset::ModelData* data = skeletonOf(rt, x, &scale);
        if (data == nullptr) return api::Value(false);
        return api::Value(ecs::suggestCreatureIK(*data, ikOf(x)));
    }, {"", "Configura el IK solo (patas, cuello, cola)", "booleano"});
    api.method("Entity", "setupRagdoll", [&rt](api::Call& c) {
        ecs::Entity x = rt.selfEntity(c);
        float scale = 1.0f;
        const asset::ModelData* data = skeletonOf(rt, x, &scale);
        if (data == nullptr) return api::Value(0);
        ecs::Ragdoll& rag = x.has<ecs::Ragdoll>() ? x.get<ecs::Ragdoll>() : x.add<ecs::Ragdoll>();
        rag.bones = ecs::suggestRagdollBones(*data, scale);
        return api::Value(static_cast<int>(rag.bones.size()));
    }, {"", "Genera los huesos del ragdoll", "numero de huesos"});
    api.method("Entity", "setupPhysBones", [&rt](api::Call& c) {
        ecs::Entity x = rt.selfEntity(c);
        const asset::ModelData* data = skeletonOf(rt, x, nullptr);
        if (data == nullptr) return api::Value(0);
        ecs::PhysBones& pb = x.has<ecs::PhysBones>() ? x.get<ecs::PhysBones>() : x.add<ecs::PhysBones>();
        pb.chains = ecs::suggestPhysBones(*data);
        return api::Value(static_cast<int>(pb.chains.size()));
    }, {"", "Detecta pelo, colas, orejas...", "numero de cadenas"});
}

// --- Ragdoll y Bone Sockets -----------------------------------------------------------

void registerRagdoll(Runtime& rt) {
    api::NativeApi& api = rt.native;
    api.property("Entity", "ragdoll",
        [&rt](api::Call& c) {
            const ecs::Entity x = rt.selfEntity(c);
            const ecs::Ragdoll* r = x.tryGet<ecs::Ragdoll>();
            return api::Value(r != nullptr && r->active);
        },
        [&rt](api::Call& c) {
            ecs::Entity x = rt.selfEntity(c);
            const bool on = c.boolean(0);
            ecs::Ragdoll* r = x.tryGet<ecs::Ragdoll>();
            if (r == nullptr) {
                if (!on) return api::Value{};
                r = &x.add<ecs::Ragdoll>();
            }
            r->active = on;
            return api::Value{};
        },
        {"", "Ragdoll activado (true/false)", "booleano"}, true);
    // e:addRagdollForce(impulso [, "Hueso" | punto]): un golpe al caer.
    api.method("Entity", "addRagdollForce", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const Vec3 impulse = c.vec3(0);
        ecs::Ragdoll* r = x.tryGet<ecs::Ragdoll>();
        if (r == nullptr || !r->active) return api::Value(false);
        if (!r->runtime.ptr) r->runtime.ptr = std::make_shared<ecs::RagdollRuntime>();
        ecs::RagdollRuntime::Push push;
        push.impulse = impulse;
        const api::Value& where = c.arg(1);
        if (where.isString()) {
            // El hueso por su nombre, con o sin el prefijo ("mixamorig:Spine" vale como "Spine").
            const std::string bone = where.asString();
            for (std::size_t i = 0; i < r->runtime.ptr->bones.size(); ++i) {
                const std::string& n = r->runtime.ptr->bones[i].name;
                if (n == bone || (n.size() > bone.size() && n.compare(n.size() - bone.size(), bone.size(), bone) == 0)) {
                    push.bone = static_cast<int>(i);
                    break;
                }
            }
        } else if (where.isVec3()) {
            push.point = where.asVec3();
            push.at_point = true;
        }
        r->runtime.ptr->pushes.push_back(push);
        return api::Value(true);
    }, {"Vec3, \"Spine\"", "Empujon al ragdoll (en un hueso o un punto)", "booleano"});

    // espada:attachToBone(personaje, "RightHand" [, desplazamiento, giro])
    api.method("Entity", "attachToBone", [&rt](api::Call& c) {
        ecs::Entity x = rt.selfEntity(c);
        const ecs::Entity m = rt.entityArg(c, 0);
        const std::string bone = c.string(1);
        const Vec3 offset = c.vec3(2, Vec3{});
        const Vec3 rotation = c.vec3(3, Vec3{});
        if (!m.valid() || x == m) return api::Value(false);
        if (!m.isAncestorOf(x)) x.setParent(m, true);
        ecs::BoneSocket& s = x.has<ecs::BoneSocket>() ? x.get<ecs::BoneSocket>() : x.add<ecs::BoneSocket>();
        s.bone = bone;
        s.mode = ecs::SocketMode::Follow;
        s.position = offset;
        s.rotation = rotation;
        return api::Value(true);
    }, {"modelo, \"RightHand\", offset, giro", "Sigue a un hueso (Bone Socket)", "booleano"});
    api.method("Entity", "detachFromBone", [&rt](api::Call& c) {
        ecs::Entity x = rt.selfEntity(c);
        if (x.has<ecs::BoneSocket>()) x.remove<ecs::BoneSocket>();
        return api::Value{};
    }, {"", "Deja de seguir al hueso"});
}

// --- Navegacion -----------------------------------------------------------------------

void registerNavigation(Runtime& rt) {
    api::NativeApi& api = rt.native;
    // Entidades con NavAgent: como el MoveTo del AIController de Unreal.
    api.method("Entity", "moveTo", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const Vec3 target = c.vec3(0);
        return api::Value(rt.navigation != nullptr && rt.navigation->moveTo(x, target));
    }, {"Vec3", "Su NavAgent camina hasta alli por la malla", "booleano"});
    api.method("Entity", "stopMoving", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        if (rt.navigation != nullptr) rt.navigation->stop(x);
        return api::Value{};
    }, {"", "Se para"});
    api.property("Entity", "isMoving", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        return api::Value(rt.navigation != nullptr && rt.navigation->isMoving(x));
    }, {}, {"", "Su NavAgent va hacia un destino", "booleano"}, true);
    api.property("Entity", "remainingDistance", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        return api::Value(rt.navigation != nullptr ? rt.navigation->remainingDistance(x) : 0.0f);
    }, {}, {"", "Metros que le quedan por el camino", "numero"}, true);
    api.property("Entity", "navVelocity", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        return api::Value(rt.navigation != nullptr ? rt.navigation->agentVelocity(x) : Vec3{});
    }, {}, {"", "Velocidad de su NavAgent", "Vec3"}, true);

    // Navigation: consultas de la malla.
    api.function("Navigation.isReady", [&rt](api::Call&) {
        return api::Value(rt.navigation != nullptr && rt.navigation->ready());
    }, {"", "Hay malla de navegacion?", "booleano"});
    api.function("Navigation.findPath", [&rt](api::Call& c) {
        const Vec3 from = c.vec3(0);
        const Vec3 to = c.vec3(1);
        std::vector<Vec3> path;
        if (rt.navigation == nullptr || !rt.navigation->findPath(from, to, path)) return api::Value{};
        api::Value::Array out;
        out.reserve(path.size());
        for (const Vec3& p : path) out.emplace_back(p);
        return api::Value(std::move(out));
    }, {"desde, hasta", "Camino por la malla (los puntos de giro)", "lista de Vec3 o nil"});
    api.function("Navigation.projectPoint", [&rt](api::Call& c) {
        const Vec3 point = c.vec3(0);
        const float extent = static_cast<float>(c.number(1, 2.0));
        Vec3 result{};
        if (rt.navigation == nullptr || !rt.navigation->projectPoint(point, result, extent)) return api::Value{};
        return api::Value(result);
    }, {"Vec3, radio", "El punto de la malla mas cercano", "Vec3 o nil"});
    api.function("Navigation.randomPoint", [&rt](api::Call& c) {
        const Vec3 center = c.vec3(0);
        const float radius = static_cast<float>(c.number(1));
        Vec3 result{};
        if (rt.navigation == nullptr || !rt.navigation->randomPoint(center, radius, result)) return api::Value{};
        return api::Value(result);
    }, {"centro, radio", "Un punto al azar de la malla", "Vec3 o nil"});
    // [true, hasta] si la linea recta por la malla llega; si no, [false, punto del choque].
    api.function("Navigation.raycast", [&rt](api::Call& c) {
        const Vec3 from = c.vec3(0);
        const Vec3 to = c.vec3(1);
        Vec3 hit = to;
        const bool clear = rt.navigation != nullptr && rt.navigation->raycast(from, to, &hit);
        return api::Value(api::Value::Array{api::Value(clear), api::Value(hit)});
    }, {"desde, hasta", "Linea recta por la malla: llega?, punto del choque", "lista [booleano, Vec3]"});
}

// --- Character Controller -------------------------------------------------------------
//
//   flags = entity:move(Vec3(0, 0, -speed * dt))   -- Move de Unity
//   entity:isGrounded() / entity:setMoveInput(dir, corriendo) / entity:jump()
//   entity:characterState()                       -- grounded, velocity, ground...
//
// CharacterController.Sides / Above / Below: bits de lo que devuelve move()
// (y de characterState().flags).

void registerCharacter(Runtime& rt) {
    api::NativeApi& api = rt.native;
    const auto constant = [&api](const char* name, std::uint32_t bit, const char* what) {
        api.property("CharacterController", name, [bit](api::Call&) { return api::Value(bit); }, {},
                     {"", what, "numero"});
    };
    constant("Sides", physics::PhysicsSystem::kCollidedSides, "Bit de move(): choco por los lados");
    constant("Above", physics::PhysicsSystem::kCollidedAbove, "Bit de move(): choco por arriba");
    constant("Below", physics::PhysicsSystem::kCollidedBelow, "Bit de move(): toca el suelo");

    api.method("Entity", "move", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const Vec3 displacement = c.vec3(0);
        if (rt.physics == nullptr) return api::Value(0);
        return api::Value(rt.physics->moveCharacter(rt.requireWorld(), x, displacement));
    }, {"Vec3", "Mueve su CharacterController ya, chocando y deslizando (Move de Unity)",
        "numero (bits de CharacterController.Sides / Above / Below)"});
    api.method("Entity", "isGrounded", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        return api::Value(rt.physics != nullptr && rt.physics->characterState(x).grounded);
    }, {"", "Su CharacterController esta en el suelo?", "booleano"});
    api.method("Entity", "isCrouching", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        return api::Value(rt.physics != nullptr && rt.physics->characterState(x).crouching);
    }, {"", "Su CharacterController esta agachado?", "booleano"});
    api.method("Entity", "setMoveInput", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const Vec3 direction = c.vec3(0);
        const bool run = c.boolean(1, false);
        if (rt.physics != nullptr) rt.physics->setCharacterInput(x, direction, run);
        return api::Value{};
    }, {"Vec3, corriendo", "Movimiento integrado: direccion en el mundo (0..1) hasta la siguiente llamada"});
    api.method("Entity", "jump", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const float height = static_cast<float>(c.number(0, -1.0));
        return api::Value(rt.physics != nullptr && rt.physics->characterJump(x, height));
    }, {"altura", "Salta (sin altura: la del componente)", "booleano (false si no puede)"});
    api.method("Entity", "setCrouch", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const bool crouch = c.boolean(0);
        if (rt.physics != nullptr) rt.physics->setCharacterCrouch(x, crouch);
        return api::Value{};
    }, {"true", "Se agacha (o se levanta si cabe)"});
    api.method("Entity", "addCharacterVelocity", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        const Vec3 velocity = c.vec3(0);
        if (rt.physics != nullptr) rt.physics->addCharacterVelocity(x, velocity);
        return api::Value{};
    }, {"Vec3", "Suma velocidad a su CharacterController (empujones, saltos)"});
    api.method("Entity", "characterState", [&rt](api::Call& c) {
        const ecs::Entity x = rt.selfEntity(c);
        if (rt.physics == nullptr) return api::Value{};
        const physics::PhysicsSystem::CharacterState s = rt.physics->characterState(x);
        if (!s.valid) return api::Value{};
        static constexpr const char* kGround[] = {"OnGround", "OnSteepGround", "NotSupported", "InAir"};
        api::Value t = api::Value::object();
        t.set("grounded", s.grounded);
        t.set("groundState", kGround[static_cast<int>(s.ground_state)]);
        t.set("crouching", s.crouching);
        t.set("velocity", s.velocity);
        t.set("groundNormal", s.ground_normal);
        t.set("groundPoint", s.ground_point);
        t.set("groundVelocity", s.ground_velocity);
        if (s.ground.valid()) t.set("ground", rt.entityValue(s.ground));
        t.set("flags", s.collision_flags);
        t.set("jumps", s.jumps_used);
        return t;
    }, {"", "Estado de su CharacterController",
        "nil o {grounded, groundState, crouching, velocity, groundNormal, groundPoint, groundVelocity, ground, flags, "
        "jumps}"});
}

}  // namespace

void registerRigApi(Runtime& rt) {
    registerBones(rt);
    registerIK(rt);
    registerRagdoll(rt);
    registerNavigation(rt);
    registerCharacter(rt);
}

}  // namespace cramion::scripting::native
