// Pruebas de native/RigApi.cpp: huesos, IK, ragdoll y sockets de Entity, la
// navegacion sin malla (la de verdad esta en NavigationTests) y el Character
// Controller (tabla CharacterController y Entity:move, isGrounded, jump...).

#include "ApiTest.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/physics/Ragdoll.h"

#include <CramionFX/asset/Model.h>

#include <cmath>
#include <memory>

using namespace cramion;
using namespace cramion::apitest;
using core::Vec3;
using Error = scripting::api::Error;

namespace {

bool nearly(const Vec3& a, const Vec3& b, float eps = 1e-3f) { return core::length(a - b) < eps; }

// Esqueleto de prueba: Root -> Tail -> Tail2 (una cola para los Phys Bones).
asset::ModelData tailModel() {
    asset::ModelData m;
    asset::Node root;
    root.name = "Root";
    asset::Node tail;
    tail.name = "Tail";
    tail.parent = 0;
    tail.local = core::translate(Vec3{0.0f, 0.0f, -0.3f});
    asset::Node tail2 = tail;
    tail2.name = "Tail2";
    tail2.parent = 1;
    m.nodes = {root, tail, tail2};
    return m;
}

void testBones() {
    std::printf("Huesos\n");
    ApiFixture t(false);
    static const asset::ModelData model = tailModel();
    const core::Quat head_rotation = ecs::quatFromEulerDegrees(Vec3{0.0f, 90.0f, 0.0f});
    scripting::ScriptSystem::SkeletonHost host;
    host.bone_world = [&](ecs::Entity, const std::string& bone, core::Mat4& world) {
        if (bone != "Head") return false;
        world = core::composeTrs(Vec3{1.0f, 2.0f, 3.0f}, head_rotation, Vec3{1.0f, 1.0f, 1.0f});
        return true;
    };
    host.bone_names = [](ecs::Entity) { return std::vector<std::string>{"Hips", "Head"}; };
    host.skeleton = [](ecs::Entity e, float* scale) -> const asset::ModelData* {
        if (scale != nullptr) *scale = 1.0f;
        return e.name() == "Modelo" ? &model : nullptr;
    };
    t.scripts.setSkeletonHost(host);
    t.scripts.start(t.world);
    ecs::Entity e = t.world.create("Modelo");
    const Value self = t.entity(e);

    const Value bones = t.call("getBones", {}, self);
    check(bones.isArray() && bones.size() == 2 && bones[1].asString() == "Head", "getBones da los nombres");
    const Value p = t.call("getBonePosition", {Value("Head")}, self);
    check(p.isVec3() && nearly(p.asVec3(), Vec3{1, 2, 3}), "getBonePosition en el mundo");
    check(t.call("getBonePosition", {Value("Nada")}, self).isNil(), "un hueso que no hay da nil");
    const Value r = t.call("getBoneRotation", {Value("Head")}, self);
    const core::Quat q = r.asQuat();
    const float dot = q.x * head_rotation.x + q.y * head_rotation.y + q.z * head_rotation.z + q.w * head_rotation.w;
    check(r.isQuat() && std::abs(dot) > 0.999f, "getBoneRotation da el giro del hueso");
    const json bridged = t.bridge({{"fn", "getBonePosition"}, {"self", t.api().toJson(self)}, {"args", {"Head"}}});
    check(bridged["ok"] == true && bridged["result"]["$v"][2].get<float>() == 3.0f, "getBonePosition por el puente");

    t.call("setBoneRotation", {Value("Head"), Value(Vec3{10.0f, 0.0f, 0.0f}), Value(0.5)}, self);
    const ecs::Skeleton* sk = e.tryGet<ecs::Skeleton>();
    check(sk != nullptr && sk->bones.size() == 1 && sk->bones[0].bone == "Head" &&
              nearly(sk->bones[0].rotation, Vec3{10, 0, 0}) && sk->bones[0].weight == 0.5f,
          "setBoneRotation (grados y peso) anade el Skeleton");
    t.call("setBoneRotation", {Value("Head"), Value(ecs::quatFromEulerDegrees(Vec3{0.0f, 30.0f, 0.0f}))}, self);
    check(nearly(e.get<ecs::Skeleton>().bones[0].rotation, Vec3{0, 30, 0}, 0.01f) &&
              e.get<ecs::Skeleton>().bones[0].weight == 0.5f,
          "setBoneRotation con un Quat (el peso sigue)");
    t.call("setBoneOffset", {Value("Hips"), Value(Vec3{0.0f, 0.1f, 0.0f})}, self);
    t.call("setBoneScale", {Value("Head"), Value(1.5)}, self);
    t.call("setBoneScale", {Value("Hips"), Value(Vec3{1.0f, 2.0f, 1.0f})}, self);
    const auto& list = e.get<ecs::Skeleton>().bones;
    check(list.size() == 2 && nearly(list[1].position, Vec3{0, 0.1f, 0}) && nearly(list[0].scale, Vec3{1.5f, 1.5f, 1.5f}) &&
              nearly(list[1].scale, Vec3{1, 2, 1}),
          "setBoneOffset y setBoneScale (numero o Vec3)");
    t.call("resetBone", {Value("Head")}, self);
    check(e.get<ecs::Skeleton>().bones.size() == 1 && e.get<ecs::Skeleton>().bones[0].bone == "Hips", "resetBone");
    t.call("resetBones", {}, self);
    check(e.get<ecs::Skeleton>().bones.empty(), "resetBones");
    t.call("showBones", {Value(false)}, self);
    check(!e.get<ecs::Skeleton>().show_bones, "showBones(false)");
    t.call("showBones", {}, self);
    check(e.get<ecs::Skeleton>().show_bones, "showBones() los ensena");

    // Configuracion automatica.
    check(t.call("setupPhysBones", {}, self).asNumber() == 1.0 && e.has<ecs::PhysBones>() &&
              e.get<ecs::PhysBones>().chains[0].bone == "Tail",
          "setupPhysBones detecta la cola");
    const Value ragdoll_bones = t.call("setupRagdoll", {}, self);
    check(ragdoll_bones.isNumber() && e.has<ecs::Ragdoll>() &&
              ragdoll_bones.asNumber() == static_cast<double>(e.get<ecs::Ragdoll>().bones.size()),
          "setupRagdoll rellena los huesos del Ragdoll");
    check(t.call("setupCreatureIK", {}, self).isBool(), "setupCreatureIK devuelve si reconoce el esqueleto");
    ecs::Entity other = t.world.create("Otro");
    check(t.call("setupCreatureIK", {}, t.entity(other)).truthy() == false &&
              t.call("setupRagdoll", {}, t.entity(other)).asNumber() == 0.0 && !other.has<ecs::Ragdoll>(),
          "sin esqueleto: false y 0");

    // Una entidad destruida da error (antes no hacia nada).
    t.world.destroy(other);
    bool threw = false;
    try {
        t.call("getBones", {}, t.entity(other));
    } catch (const Error&) {
        threw = true;
    }
    check(threw, "una entidad que ya no existe da error");
}

void testIK() {
    std::printf("IK\n");
    ApiFixture t;
    ecs::Entity e = t.world.create("Personaje");
    ecs::Entity target = t.world.create("Objetivo");
    ecs::Entity pole = t.world.create("Pole");
    const Value self = t.entity(e);

    t.call("setIKTarget", {Value("left_hand"), t.entity(target)}, self);
    const ecs::InverseKinematics* ik = e.tryGet<ecs::InverseKinematics>();
    check(ik != nullptr && ik->left_hand.target == target.uuid() && !ik->left_hand.use_position,
          "setIKTarget con una entidad anade el IK");
    t.call("setIKTarget", {Value("RightFoot"), Value(Vec3{1.0f, 0.0f, 2.0f})}, self);
    ik = &e.get<ecs::InverseKinematics>();
    check(ik->right_foot.use_position && nearly(ik->right_foot.position, Vec3{1, 0, 2}), "setIKTarget con un punto");
    t.call("setIKTarget", {Value("left_hand"), Value()}, self);
    check(!e.get<ecs::InverseKinematics>().left_hand.target.valid(), "setIKTarget con nil lo quita");
    t.call("setIKTarget", {Value("Cola"), Value(Vec3{0.0f, 1.0f, 0.0f}), Value(3)}, self);
    ik = &e.get<ecs::InverseKinematics>();
    check(ik->chains.size() == 1 && ik->chains[0].bone == "Cola" && ik->chains[0].length == 3 &&
              ik->chains[0].use_position && !ik->chains[0].ground,
          "setIKTarget de otro hueso crea una cadena");
    t.call("setIKTarget", {Value("Cola"), Value(Vec3{}), Value(40)}, self);
    check(e.get<ecs::InverseKinematics>().chains[0].length == 16, "la longitud se limita a 16");
    t.call("setIKHint", {Value("Cola"), t.entity(pole)}, self);
    t.call("setIKHint", {Value("left_hand"), t.entity(pole)}, self);
    ik = &e.get<ecs::InverseKinematics>();
    check(ik->chains[0].hint == pole.uuid() && ik->left_hand.hint == pole.uuid(), "setIKHint");
    t.call("setIKWeight", {Value("look"), Value(2.0)}, self);
    t.call("setIKWeight", {Value("feet"), Value(0.3)}, self);
    t.call("setIKWeight", {Value("Cola"), Value(0.25)}, self);
    t.call("setIKWeight", {Value("left_hand"), Value(-1.0)}, self);
    ik = &e.get<ecs::InverseKinematics>();
    check(ik->look_weight == 1.0f && std::abs(ik->grounding_weight - 0.3f) < 1e-6f && ik->chains[0].weight == 0.25f &&
              ik->left_hand.weight == 0.0f,
          "setIKWeight (limitado a 0..1; look, feet y cadenas)");
    t.call("setLookAt", {Value(Vec3{0.0f, 2.0f, 5.0f}), Value(0.4)}, self);
    ik = &e.get<ecs::InverseKinematics>();
    check(ik->look_use_position && nearly(ik->look_position, Vec3{0, 2, 5}) && std::abs(ik->look_weight - 0.4f) < 1e-6f,
          "setLookAt con un punto y peso");
    t.call("setLookAt", {t.entity(target)}, self);
    check(e.get<ecs::InverseKinematics>().look_at == target.uuid() && !e.get<ecs::InverseKinematics>().look_use_position,
          "setLookAt con una entidad");

    ecs::IKChain foot;
    foot.bone = "Pata";
    foot.ground = true;
    foot.weight = 0.0f;
    e.get<ecs::InverseKinematics>().chains.push_back(foot);
    t.call("setFootGrounding", {Value(true)}, self);
    ik = &e.get<ecs::InverseKinematics>();
    check(ik->foot_grounding && ik->chains[1].weight == 1.0f && ik->chains[0].weight == 0.25f,
          "setFootGrounding(true) activa las cadenas al suelo");
    t.call("setFootGrounding", {Value(false)}, self);
    check(!e.get<ecs::InverseKinematics>().foot_grounding && e.get<ecs::InverseKinematics>().chains[1].weight == 0.0f,
          "setFootGrounding(false)");
    const json r = t.bridge({{"fn", "Entity:setIKTarget"}, {"self", t.api().toJson(self)},
                             {"args", {"right_hand", {{"$v", {1, 2, 3}}}}}});
    check(r["ok"] == true && nearly(e.get<ecs::InverseKinematics>().right_hand.position, Vec3{1, 2, 3}),
          "setIKTarget por el puente");
}

void testRagdollAndSockets() {
    std::printf("Ragdoll y Bone Sockets\n");
    ApiFixture t;
    ecs::Entity e = t.world.create("Muneco");
    const Value self = t.entity(e);
    check(t.get("ragdoll", self).isBool() && !t.get("ragdoll", self).truthy(), "ragdoll sin componente es false");
    t.set("ragdoll", Value(false), self);
    check(!e.has<ecs::Ragdoll>(), "ragdoll = false no anade el componente");
    check(t.call("addRagdollForce", {Value(Vec3{0.0f, 0.0f, 5.0f})}, self).truthy() == false,
          "addRagdollForce sin ragdoll activo da false");
    t.set("Entity:ragdoll", Value(true), self);
    check(e.has<ecs::Ragdoll>() && e.get<ecs::Ragdoll>().active && t.get("ragdoll", self).truthy(), "ragdoll = true");
    ecs::Ragdoll& rag = e.get<ecs::Ragdoll>();
    rag.runtime.ptr = std::make_shared<ecs::RagdollRuntime>();
    rag.runtime.ptr->bones.resize(3);
    rag.runtime.ptr->bones[0].name = "mixamorig:Hips";
    rag.runtime.ptr->bones[1].name = "mixamorig:Spine";
    rag.runtime.ptr->bones[2].name = "mixamorig:Head";
    check(t.call("addRagdollForce", {Value(Vec3{0.0f, 0.0f, 5.0f}), Value("Spine")}, self).truthy(),
          "addRagdollForce con ragdoll activo");
    t.call("addRagdollForce", {Value(Vec3{1.0f, 0.0f, 0.0f}), Value(Vec3{0.0f, 1.0f, 0.0f})}, self);
    t.call("addRagdollForce", {Value(Vec3{0.0f, 2.0f, 0.0f})}, self);
    const auto& pushes = rag.runtime.ptr->pushes;
    check(pushes.size() == 3 && pushes[0].bone == 1 && nearly(pushes[0].impulse, Vec3{0, 0, 5}) && !pushes[0].at_point,
          "en un hueso por su nombre (sin el prefijo)");
    check(pushes[1].at_point && nearly(pushes[1].point, Vec3{0, 1, 0}) && pushes[2].bone == -1 && !pushes[2].at_point,
          "en un punto o sin donde");
    t.set("ragdoll", Value(false), self);
    check(!e.get<ecs::Ragdoll>().active, "ragdoll = false lo apaga");

    ecs::Entity hero = t.world.create("Heroe");
    ecs::Entity sword = t.world.create("Espada");
    const Value sw = t.entity(sword);
    check(t.call("attachToBone", {t.entity(hero), Value("RightHand"), Value(Vec3{0.0f, 0.1f, 0.0f}),
                                  Value(Vec3{0.0f, 0.0f, 90.0f})}, sw).truthy(),
          "attachToBone devuelve true");
    const ecs::BoneSocket* socket = sword.tryGet<ecs::BoneSocket>();
    check(sword.parent() == hero && socket != nullptr && socket->bone == "RightHand" &&
              socket->mode == ecs::SocketMode::Follow && nearly(socket->position, Vec3{0, 0.1f, 0}) &&
              nearly(socket->rotation, Vec3{0, 0, 90}),
          "la espada pasa a ser hija del modelo con su Bone Socket");
    check(!t.call("attachToBone", {sw, Value("RightHand")}, sw).truthy() &&
              !t.call("attachToBone", {Value(), Value("RightHand")}, sw).truthy(),
          "attachToBone a si misma o a nil da false");
    t.call("detachFromBone", {}, sw);
    check(!sword.has<ecs::BoneSocket>(), "detachFromBone quita el socket");
}

void testNavigationWithoutMesh() {
    std::printf("Navegacion sin sistema\n");
    ApiFixture t;
    ecs::Entity e = t.world.create("Agente");
    const Value self = t.entity(e);
    check(!t.call("Navigation.isReady").truthy(), "isReady es false");
    check(t.call("Navigation.findPath", {Value(Vec3{}), Value(Vec3{1.0f, 0.0f, 0.0f})}).isNil(), "findPath da nil");
    check(t.call("Navigation.projectPoint", {Value(Vec3{})}).isNil() &&
              t.call("Navigation.randomPoint", {Value(Vec3{}), Value(3)}).isNil(),
          "projectPoint y randomPoint dan nil");
    const json r = t.bridge({{"fn", "Navigation.raycast"}, {"args", {{{"$v", {0, 0, 0}}}, {{"$v", {4, 0, 0}}}}}});
    check(r["ok"] == true && r["result"].is_array() && r["result"][0] == false && r["result"][1]["$v"][0] == 4.0,
          "raycast da [llega, punto] (dos valores en una lista)");
    check(!t.call("moveTo", {Value(Vec3{1.0f, 0.0f, 0.0f})}, self).truthy(), "moveTo da false");
    t.call("stopMoving", {}, self);
    check(!t.get("isMoving", self).truthy() && t.get("remainingDistance", self).asNumber() == 0.0 &&
              t.get("navVelocity", self).isVec3(),
          "isMoving, remainingDistance y navVelocity");
    bool threw = false;
    try {
        t.set("isMoving", Value(true), self);
    } catch (const Error&) {
        threw = true;
    }
    check(threw, "isMoving es de solo lectura");
    bool missing = false;
    try {
        t.call("Navigation.findPath", {Value(Vec3{})});
    } catch (const Error&) {
        missing = true;
    }
    check(missing, "findPath sin destino da error");
}

void testCharacter() {
    std::printf("Character Controller\n");
    {
        ApiFixture t;
        ecs::Entity e = t.world.create("Sin fisica");
        const Value self = t.entity(e);
        check(t.get("CharacterController.Sides").asNumber() == 1.0 && t.get("CharacterController.Above").asNumber() == 2.0 &&
                  t.get("CharacterController.Below").asNumber() == 4.0,
              "CharacterController.Sides / Above / Below");
        check(t.call("move", {Value(Vec3{1.0f, 0.0f, 0.0f})}, self).asNumber() == 0.0 &&
                  !t.call("isGrounded", {}, self).truthy() && t.call("characterState", {}, self).isNil() &&
                  !t.call("jump", {}, self).truthy(),
              "sin fisica: 0, false y nil");
    }
    physics::PhysicsSystem physics;
    ApiFixture t(false);
    t.scripts.setPhysics(&physics);
    t.world.create("Suelo").add<physics::PlaneCollider>();
    ecs::Entity hero = t.world.create("Heroe");
    hero.setWorldPosition(Vec3{0.0f, 1.0f, 0.0f});
    hero.add<physics::CharacterController>().keyboard = false;
    ecs::Entity manual = t.world.create("Manual");
    manual.setWorldPosition(Vec3{10.0f, 0.0f, 0.0f});
    auto& mc = manual.add<physics::CharacterController>();
    mc.keyboard = false;
    mc.movement = physics::CharacterMovement::Manual;
    ecs::Entity plain = t.world.create("Caja");
    physics.start(t.world);
    t.scripts.start(t.world);
    const auto run = [&](float seconds) {
        for (int i = 0; i < static_cast<int>(seconds * 60.0f); ++i) {
            physics.update(t.world, 1.0f / 60.0f);
            t.frame();
        }
    };
    run(1.5f);
    const Value self = t.entity(hero);
    check(t.call("isGrounded", {}, self).truthy() && !t.call("isCrouching", {}, self).truthy(), "isGrounded en el suelo");
    const Value s = t.call("characterState", {}, self);
    check(s.isObject() && s["grounded"].truthy() && s["groundState"].asString() == "OnGround" && s["velocity"].isVec3() &&
              s["groundNormal"].isVec3() && s["flags"].isNumber() && s["jumps"].isNumber(),
          "characterState da su estado");
    check(t.call("characterState", {}, t.entity(plain)).isNil(), "characterState de algo sin CharacterController es nil");
    const json state = t.bridge({{"fn", "characterState"}, {"self", t.api().toJson(self)}});
    check(state["ok"] == true && state["result"]["grounded"] == true && state["result"]["groundPoint"].contains("$v"),
          "characterState por el puente");

    t.call("setMoveInput", {Value(Vec3{1.0f, 0.0f, 0.0f})}, self);
    run(0.5f);
    const float x0 = hero.worldPosition().x;
    check(x0 > 0.5f, "setMoveInput lo hace andar");
    t.call("setMoveInput", {Value(Vec3{1.0f, 0.0f, 0.0f}), Value(true)}, self);
    run(0.5f);
    check(hero.worldPosition().x - x0 > 2.5f, "setMoveInput corriendo va mas rapido");
    t.call("setMoveInput", {Value(Vec3{})}, self);
    run(0.5f);
    check(t.call("jump", {}, self).truthy(), "jump en el suelo");
    float top = 0.0f;
    for (int i = 0; i < 30; ++i) {
        run(1.0f / 60.0f + 1e-4f);
        top = std::max(top, hero.worldPosition().y);
    }
    check(top > 0.5f && !t.call("isGrounded", {}, self).truthy(), "salta y deja el suelo");
    run(1.5f);
    t.call("setCrouch", {Value(true)}, self);
    run(0.3f);
    check(t.call("isCrouching", {}, self).truthy(), "setCrouch(true) lo agacha");
    t.call("setCrouch", {Value(false)}, self);
    run(0.3f);
    check(!t.call("isCrouching", {}, self).truthy(), "setCrouch(false) lo levanta");
    t.call("addCharacterVelocity", {Value(Vec3{0.0f, 6.0f, 0.0f})}, self);
    run(0.1f);
    check(hero.worldPosition().y > 0.2f, "addCharacterVelocity lo empuja");

    const Value m = t.entity(manual);
    const Value flags = t.call("move", {Value(Vec3{0.0f, -1.0f, -0.5f})}, m);
    check(flags.isNumber() && (static_cast<int>(flags.asNumber()) & 4) != 0 && manual.worldPosition().z < -0.4f,
          "move devuelve Below contra el suelo y desplaza");

    t.scripts.stop();
    physics.stop();
}

}  // namespace

int main() {
    testBones();
    testIK();
    testRagdollAndSockets();
    testNavigationWithoutMesh();
    testCharacter();
    return finish();
}
