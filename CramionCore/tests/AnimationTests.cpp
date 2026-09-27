// Humanoides (deteccion y retargeting) y cinematica inversa.

#include "CramionCore/anim/Humanoid.h"
#include "CramionCore/anim/IK.h"
#include "CramionCore/anim/Procedural.h"
#include "CramionCore/anim/Creature.h"
#include "CramionCore/anim/PhysBones.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/physics/Ragdoll.h"
#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/ecs/AnimatorController.h"
#include "CramionCore/ecs/MathUtil.h"

#include <CramionFX/anim/Animator.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace cramion;
using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

Quat axisAngle(const Vec3& axis, float degrees) {
    const float half = degrees * core::kPi / 360.0f;
    const Vec3 a = core::normalize(axis) * std::sin(half);
    return Quat{a.x, a.y, a.z, std::cos(half)};
}
Mat4 rotation(const Quat& q) { return core::composeTrs(Vec3{}, q, Vec3{1.0f, 1.0f, 1.0f}); }
Vec3 positionOf(const Mat4& m) { return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]}; }

// Esqueleto humano (Y arriba, mirando a +Z, la izquierda en +X) con los
// nombres de `names` (clave: hueso humano). `arm_drop` baja los brazos
// (0 = pose T, 45 = pose A); `height` escala el cuerpo; `root` va en un nodo
// raiz "Armature" (ejes y escala de otro programa); `twist` da a cada hueso
// un giro local arbitrario (cada programa orienta sus huesos a su manera).
std::vector<asset::Node> makeSkeleton(const std::map<std::string, std::string>& names, float arm_drop, float height,
                                      const Mat4& root, bool twist) {
    struct Joint {
        std::string key;
        std::string parent;
        Vec3 position;
    };
    const float c = std::cos(arm_drop * core::kPi / 180.0f);
    const float s = std::sin(arm_drop * core::kPi / 180.0f);
    const auto arm = [&](float side, float along) { return Vec3{side * (0.2f + along * c), 1.45f - along * s, 0.0f}; };
    const std::vector<Joint> joints = {
        {"Hips", "", {0.0f, 1.0f, 0.0f}},
        {"Spine", "Hips", {0.0f, 1.1f, 0.0f}},
        {"Chest", "Spine", {0.0f, 1.3f, 0.0f}},
        {"Neck", "Chest", {0.0f, 1.5f, 0.0f}},
        {"Head", "Neck", {0.0f, 1.62f, 0.0f}},
        {"LeftShoulder", "Chest", {0.05f, 1.45f, 0.0f}},
        {"LeftUpperArm", "LeftShoulder", arm(1.0f, 0.0f)},
        {"LeftLowerArm", "LeftUpperArm", arm(1.0f, 0.28f)},
        {"LeftHand", "LeftLowerArm", arm(1.0f, 0.54f)},
        {"RightShoulder", "Chest", {-0.05f, 1.45f, 0.0f}},
        {"RightUpperArm", "RightShoulder", arm(-1.0f, 0.0f)},
        {"RightLowerArm", "RightUpperArm", arm(-1.0f, 0.28f)},
        {"RightHand", "RightLowerArm", arm(-1.0f, 0.54f)},
        {"LeftUpperLeg", "Hips", {0.1f, 0.95f, 0.0f}},
        {"LeftLowerLeg", "LeftUpperLeg", {0.1f, 0.52f, 0.02f}},
        {"LeftFoot", "LeftLowerLeg", {0.1f, 0.08f, 0.0f}},
        {"LeftToes", "LeftFoot", {0.1f, 0.0f, 0.12f}},
        {"RightUpperLeg", "Hips", {-0.1f, 0.95f, 0.0f}},
        {"RightLowerLeg", "RightUpperLeg", {-0.1f, 0.52f, 0.02f}},
        {"RightFoot", "RightLowerLeg", {-0.1f, 0.08f, 0.0f}},
        {"RightToes", "RightFoot", {-0.1f, 0.0f, 0.12f}},
    };
    std::vector<asset::Node> nodes;
    std::vector<Mat4> globals;
    std::map<std::string, int> index;
    nodes.push_back(asset::Node{"Armature", -1, root});
    globals.push_back(root);
    int n = 0;
    for (const Joint& j : joints) {
        const int parent = j.parent.empty() ? 0 : index.at(j.parent);
        Quat r{};
        if (twist) r = axisAngle(Vec3{0.3f + 0.1f * n, 1.0f, -0.5f + 0.07f * n}, 37.0f * static_cast<float>(n + 1));
        const Mat4 global = root * core::translate(j.position * height) * rotation(r);
        asset::Node node;
        node.name = names.at(j.key);
        node.parent = parent;
        node.local = core::inverse(globals[static_cast<std::size_t>(parent)]) * global;
        index[j.key] = static_cast<int>(nodes.size());
        nodes.push_back(node);
        globals.push_back(global);
        ++n;
    }
    return nodes;
}

const std::map<std::string, std::string> kMixamo = {
    {"Hips", "mixamorig:Hips"},           {"Spine", "mixamorig:Spine"},          {"Chest", "mixamorig:Spine1"},
    {"Neck", "mixamorig:Neck"},           {"Head", "mixamorig:Head"},            {"LeftShoulder", "mixamorig:LeftShoulder"},
    {"LeftUpperArm", "mixamorig:LeftArm"}, {"LeftLowerArm", "mixamorig:LeftForeArm"}, {"LeftHand", "mixamorig:LeftHand"},
    {"RightShoulder", "mixamorig:RightShoulder"}, {"RightUpperArm", "mixamorig:RightArm"},
    {"RightLowerArm", "mixamorig:RightForeArm"}, {"RightHand", "mixamorig:RightHand"},
    {"LeftUpperLeg", "mixamorig:LeftUpLeg"}, {"LeftLowerLeg", "mixamorig:LeftLeg"}, {"LeftFoot", "mixamorig:LeftFoot"},
    {"LeftToes", "mixamorig:LeftToeBase"}, {"RightUpperLeg", "mixamorig:RightUpLeg"}, {"RightLowerLeg", "mixamorig:RightLeg"},
    {"RightFoot", "mixamorig:RightFoot"}, {"RightToes", "mixamorig:RightToeBase"}};
const std::map<std::string, std::string> kUnreal = {
    {"Hips", "pelvis"},         {"Spine", "spine_01"},        {"Chest", "spine_02"},       {"Neck", "neck_01"},
    {"Head", "head"},           {"LeftShoulder", "clavicle_l"}, {"LeftUpperArm", "upperarm_l"}, {"LeftLowerArm", "lowerarm_l"},
    {"LeftHand", "hand_l"},     {"RightShoulder", "clavicle_r"}, {"RightUpperArm", "upperarm_r"},
    {"RightLowerArm", "lowerarm_r"}, {"RightHand", "hand_r"}, {"LeftUpperLeg", "thigh_l"}, {"LeftLowerLeg", "calf_l"},
    {"LeftFoot", "foot_l"},     {"LeftToes", "ball_l"},      {"RightUpperLeg", "thigh_r"}, {"RightLowerLeg", "calf_r"},
    {"RightFoot", "foot_r"},    {"RightToes", "ball_r"}};
const std::map<std::string, std::string> kBlender = {
    {"Hips", "hips"},             {"Spine", "spine"},              {"Chest", "chest"},           {"Neck", "neck"},
    {"Head", "head"},             {"LeftShoulder", "shoulder.L"},  {"LeftUpperArm", "upper_arm.L"},
    {"LeftLowerArm", "forearm.L"}, {"LeftHand", "hand.L"},         {"RightShoulder", "shoulder.R"},
    {"RightUpperArm", "upper_arm.R"}, {"RightLowerArm", "forearm.R"}, {"RightHand", "hand.R"},
    {"LeftUpperLeg", "thigh.L"},  {"LeftLowerLeg", "shin.L"},      {"LeftFoot", "foot.L"},       {"LeftToes", "toe.L"},
    {"RightUpperLeg", "thigh.R"}, {"RightLowerLeg", "shin.R"},     {"RightFoot", "foot.R"},      {"RightToes", "toe.R"}};
const std::map<std::string, std::string> kBiped = {
    {"Hips", "Bip01 Pelvis"},        {"Spine", "Bip01 Spine"},          {"Chest", "Bip01 Spine1"},
    {"Neck", "Bip01 Neck"},          {"Head", "Bip01 Head"},            {"LeftShoulder", "Bip01 L Clavicle"},
    {"LeftUpperArm", "Bip01 L UpperArm"}, {"LeftLowerArm", "Bip01 L Forearm"}, {"LeftHand", "Bip01 L Hand"},
    {"RightShoulder", "Bip01 R Clavicle"}, {"RightUpperArm", "Bip01 R UpperArm"},
    {"RightLowerArm", "Bip01 R Forearm"}, {"RightHand", "Bip01 R Hand"}, {"LeftUpperLeg", "Bip01 L Thigh"},
    {"LeftLowerLeg", "Bip01 L Calf"}, {"LeftFoot", "Bip01 L Foot"},     {"LeftToes", "Bip01 L Toe0"},
    {"RightUpperLeg", "Bip01 R Thigh"}, {"RightLowerLeg", "Bip01 R Calf"}, {"RightFoot", "Bip01 R Foot"},
    {"RightToes", "Bip01 R Toe0"}};

int nodeNamed(const std::vector<asset::Node>& nodes, const std::string& name) {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

void testDetection() {
    std::printf("Deteccion de humanoides\n");
    const struct {
        const char* label;
        const std::map<std::string, std::string>* names;
    } rigs[] = {{"Mixamo", &kMixamo}, {"Unreal", &kUnreal}, {"Blender", &kBlender}, {"3ds Max Biped", &kBiped}};
    for (const auto& rig : rigs) {
        const std::vector<asset::Node> nodes = makeSkeleton(*rig.names, 0.0f, 1.0f, Mat4::identity(), false);
        const humanoid::Map map = humanoid::detect(nodes);
        bool right = map.valid;
        for (int b = 0; b < humanoid::kBoneCount && right; ++b) {
            const std::string key = humanoid::boneName(static_cast<humanoid::Bone>(b));
            if (key == "UpperChest") continue;
            right = map.nodes[static_cast<std::size_t>(b)] == nodeNamed(nodes, rig.names->at(key));
        }
        check(right, (std::string("todos los huesos de ") + rig.label).c_str());
    }
    // Un esqueleto que no es humano (una cola, un brazo robotico).
    std::vector<asset::Node> tail = {{"root", -1, Mat4::identity()}, {"tail_01", 0, core::translate(Vec3{0, 0, 1})},
                                     {"tail_02", 1, core::translate(Vec3{0, 0, 1})}};
    check(!humanoid::detect(tail).valid, "una cola no es humanoide");
}

// Globales de un modelo en el instante `time` de su clip 0 (con el FX).
std::vector<Mat4> poseAt(const asset::ModelData& model, float time) {
    anim::Animator animator(model);
    animator.play(0, false);
    animator.setTime(time);
    return animator.globals();
}

void testRetarget() {
    std::printf("Retargeting entre humanoides\n");
    // Origen: Mixamo, pose T, Y arriba, sin giros raros.
    asset::ModelData source;
    source.nodes = makeSkeleton(kMixamo, 0.0f, 1.0f, Mat4::identity(), false);
    // Destino: Unreal, pose A, un 20 % mas alto, en centimetros, Z arriba y
    // cada hueso con sus propios ejes.
    const Mat4 root = rotation(axisAngle(Vec3{1, 0, 0}, 90.0f)) * core::scale(Vec3{100.0f, 100.0f, 100.0f});
    asset::ModelData target;
    target.nodes = makeSkeleton(kUnreal, 45.0f, 1.2f, root, true);

    // Clip: el brazo izquierdo baja hasta apuntar al suelo y la cadera avanza 1 m.
    const int arm = nodeNamed(source.nodes, "mixamorig:LeftArm");
    const int hips = nodeNamed(source.nodes, "mixamorig:Hips");
    asset::AnimationClip clip;
    clip.name = "BajarBrazo";
    clip.duration = 1.0f;
    asset::AnimationChannel arm_channel;
    arm_channel.node = arm;
    Vec3 t{};
    Quat r{};
    Vec3 sc{};
    ecs::decomposeMatrix(source.nodes[static_cast<std::size_t>(arm)].local, t, r, sc);
    arm_channel.rotations = {{0.0f, r}, {1.0f, ecs::quatMultiply(axisAngle(Vec3{0, 0, 1}, -90.0f), r)}};
    asset::AnimationChannel hips_channel;
    hips_channel.node = hips;
    ecs::decomposeMatrix(source.nodes[static_cast<std::size_t>(hips)].local, t, r, sc);
    hips_channel.positions = {{0.0f, t}, {1.0f, t + Vec3{0.0f, 0.0f, 1.0f}}};
    clip.channels = {arm_channel, hips_channel};

    asset::AnimationClip converted;
    std::string error;
    const bool ok = humanoid::retargetClip(source.nodes, clip, target.nodes, converted, &error);
    check(ok && !converted.channels.empty(), "el clip se convierte");
    if (!ok) return;
    target.animations = {converted};

    const auto at = [&](const std::vector<Mat4>& g, const char* name) {
        return positionOf(g[static_cast<std::size_t>(nodeNamed(target.nodes, name))]);
    };
    // Direcciones del personaje destino en su modelo (Z arriba, cm).
    const Vec3 down = core::normalize(ecs::transformDirection(root, Vec3{0, -1, 0}));
    const Vec3 forward = core::normalize(ecs::transformDirection(root, Vec3{0, 0, 1}));

    const std::vector<Mat4> start = poseAt(target, 0.0f);
    const std::vector<Mat4> end = poseAt(target, 1.0f);
    // Al empezar el brazo esta en la pose T del origen (no en la A del destino).
    const Vec3 arm0 = core::normalize(at(start, "hand_l") - at(start, "upperarm_l"));
    const Vec3 side = core::normalize(ecs::transformDirection(root, Vec3{1, 0, 0}));
    std::printf("    (brazo al empezar: %.3f hacia el lado)\n", core::dot(arm0, side));
    check(core::dot(arm0, side) > 0.99f, "la pose A del destino se lleva a la T del origen");
    const Vec3 arm1 = core::normalize(at(end, "hand_l") - at(end, "upperarm_l"));
    std::printf("    (brazo al terminar: %.3f hacia abajo)\n", core::dot(arm1, down));
    check(core::dot(arm1, down) > 0.99f, "el brazo baja hasta el suelo como en el origen");
    // El otro brazo no se mueve.
    const Vec3 right0 = core::normalize(at(start, "hand_r") - at(start, "upperarm_r"));
    const Vec3 right1 = core::normalize(at(end, "hand_r") - at(end, "upperarm_r"));
    check(core::dot(right0, right1) > 0.999f, "el brazo derecho se queda quieto");
    // La cadera avanza 1 m del origen = 1.2 m del destino (mas alto) = 120 cm.
    const float moved = core::dot(at(end, "pelvis") - at(start, "pelvis"), forward);
    std::printf("    (cadera: %.1f unidades hacia delante)\n", moved);
    check(std::abs(moved - 120.0f) < 3.0f, "la cadera avanza escalada por la altura");
    // Los huesos del destino no cambian de largo.
    const float upper0 = core::length(at(start, "lowerarm_l") - at(start, "upperarm_l"));
    const float upper1 = core::length(at(end, "lowerarm_l") - at(end, "upperarm_l"));
    check(std::abs(upper0 - upper1) < 0.01f * upper0, "los huesos conservan su largo");

    // Por archivo: el .cranim guarda el esqueleto y se convierte al cargarlo.
    source.animations = {clip};
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_retarget_test.cranim";
    check(ecs::saveAnimationClip(source, clip, file), "guardar el clip de Mixamo");
    asset::AnimationClip loaded;
    check(ecs::loadAnimationClip(file, target, loaded) && loaded.channels.size() > 2,
          "cargarlo en el esqueleto de Unreal lo convierte");
    std::filesystem::remove(file);
}

void testIK() {
    std::printf("Cinematica inversa\n");
    // Brazo de 1 m + 1 m, un poco doblado.
    std::vector<asset::Node> nodes = {{"root", -1, Mat4::identity()},
                                      {"upper", 0, Mat4::identity()},
                                      {"mid", 1, core::translate(Vec3{0.1f, 1.0f, 0.0f})},
                                      {"end", 2, core::translate(Vec3{-0.1f, 1.0f, 0.0f})}};
    std::vector<Mat4> local;
    for (const asset::Node& n : nodes) local.push_back(n.local);
    std::vector<Mat4> global(nodes.size());
    ik::Pose pose{&nodes, &local, &global};
    ik::recomputeGlobals(pose);
    const float l1 = core::length(ik::nodePosition(pose, 2) - ik::nodePosition(pose, 1));
    const float l2 = core::length(ik::nodePosition(pose, 3) - ik::nodePosition(pose, 2));

    const Vec3 target{1.0f, 1.0f, 0.3f};
    const Vec3 pole{0.0f, 0.0f, 5.0f};
    check(ik::twoBone(pose, 1, 2, 3, target, &pole, 1.0f), "resuelve");
    const float error = core::length(ik::nodePosition(pose, 3) - target);
    std::printf("    (error: %.5f m)\n", error);
    check(error < 1e-3f, "la mano llega al objetivo");
    check(std::abs(core::length(ik::nodePosition(pose, 2) - ik::nodePosition(pose, 1)) - l1) < 1e-3f &&
              std::abs(core::length(ik::nodePosition(pose, 3) - ik::nodePosition(pose, 2)) - l2) < 1e-3f,
          "los huesos no se estiran");
    // El codo, hacia el pole (+Z) respecto a la linea hombro-objetivo.
    const Vec3 a = ik::nodePosition(pose, 1);
    const Vec3 elbow = ik::nodePosition(pose, 2) - a;
    const Vec3 line = core::normalize(target - a);
    const Vec3 bend = elbow - line * core::dot(elbow, line);
    check(bend.z > 0.2f, "el codo apunta al pole");

    // Fuera de alcance: se estira hacia el.
    const Vec3 far_target{0.0f, 5.0f, 0.0f};
    ik::twoBone(pose, 1, 2, 3, far_target, nullptr, 1.0f);
    const Vec3 reach = core::normalize(ik::nodePosition(pose, 3) - a);
    check(core::dot(reach, Vec3{0, 1, 0}) > 0.999f, "fuera de alcance, el brazo se estira hacia el objetivo");

    // Peso 0.5: a medio camino.
    ik::recomputeGlobals(pose);
    for (std::size_t i = 0; i < nodes.size(); ++i) local[i] = nodes[i].local;
    ik::recomputeGlobals(pose);
    const Vec3 before = ik::nodePosition(pose, 3);
    ik::twoBone(pose, 1, 2, 3, target, &pole, 0.5f);
    const Vec3 half = (before + target) * 0.5f;
    check(core::length(ik::nodePosition(pose, 3) - half) < 1e-3f, "con peso 0.5 va a medio camino");

    // Mirar: limitado al angulo maximo.
    std::vector<asset::Node> head_nodes = {{"head", -1, Mat4::identity()}};
    std::vector<Mat4> head_local = {Mat4::identity()};
    std::vector<Mat4> head_global(1);
    ik::Pose head{&head_nodes, &head_local, &head_global};
    ik::recomputeGlobals(head);
    ik::lookAt(head, 0, Vec3{0, 0, 1}, Vec3{10.0f, 0.0f, 0.0f}, 1.0f, 30.0f);
    const Vec3 facing = ecs::quatRotate(ik::nodeRotation(head, 0), Vec3{0, 0, 1});
    const float angle = std::acos(std::clamp(facing.z, -1.0f, 1.0f)) * 180.0f / core::kPi;
    std::printf("    (giro de la cabeza: %.1f grados)\n", angle);
    check(std::abs(angle - 30.0f) < 0.5f && facing.x > 0.0f, "la cabeza gira hacia el objetivo hasta 30 grados");
}

}  // namespace

void testSprings() {
    std::printf("Huesos con muelle\n");
    // Cadena horizontal de 4 huesos de 0.2 m (una cola) que sale de (0, 1, 0).
    std::vector<asset::Node> nodes = {{"root", -1, Mat4::identity()},
                                      {"tail_0", 0, core::translate(Vec3{0.0f, 1.0f, 0.0f})},
                                      {"tail_1", 1, core::translate(Vec3{0.2f, 0.0f, 0.0f})},
                                      {"tail_2", 2, core::translate(Vec3{0.2f, 0.0f, 0.0f})},
                                      {"tail_3", 3, core::translate(Vec3{0.2f, 0.0f, 0.0f})}};
    std::vector<Mat4> local(nodes.size());
    std::vector<Mat4> global(nodes.size());
    ik::Pose pose{&nodes, &local, &global};
    const auto reset = [&] {  // la pose animada de cada frame
        for (std::size_t i = 0; i < nodes.size(); ++i) local[i] = nodes[i].local;
        ik::recomputeGlobals(pose);
    };
    // Sin rigidez y con gravedad: cuelga.
    procedural::SpringChain chain = procedural::makeSpringChain(nodes, 1);
    check(chain.joints.size() == 4, "la cadena tiene la raiz y sus 3 hijos");
    procedural::SpringSettings loose;
    loose.stiffness = 0.0f;
    loose.gravity = 9.8f;
    for (int f = 0; f < 180; ++f) {
        reset();
        procedural::updateSprings(pose, Mat4::identity(), chain, loose, {}, 1.0f / 60.0f);
    }
    const Vec3 tip = ik::nodePosition(pose, 4);
    std::printf("    (punta colgando: y = %.3f)\n", tip.y);
    check(tip.y < 0.5f, "sin rigidez, la cola cuelga por su peso");
    bool lengths = true;
    for (int i = 2; i <= 4; ++i) {
        lengths = lengths && std::abs(core::length(ik::nodePosition(pose, i) - ik::nodePosition(pose, i - 1)) - 0.2f) < 0.005f;
    }
    check(lengths, "los huesos no se estiran");

    // Rigida y sin gravedad: al mover el personaje se queda atras y vuelve.
    procedural::SpringChain stiff_chain = procedural::makeSpringChain(nodes, 1);
    procedural::SpringSettings stiff;
    stiff.stiffness = 150.0f;
    stiff.gravity = 0.0f;
    stiff.damping = 0.15f;
    for (int f = 0; f < 30; ++f) {
        reset();
        procedural::updateSprings(pose, Mat4::identity(), stiff_chain, stiff, {}, 1.0f / 60.0f);
    }
    float lag = 0.0f;
    Mat4 world = Mat4::identity();
    for (int f = 0; f < 15; ++f) {  // el personaje sube 1.5 m en 0.25 s
        world = core::translate(Vec3{0.0f, 0.1f * static_cast<float>(f + 1), 0.0f});
        reset();
        procedural::updateSprings(pose, world, stiff_chain, stiff, {}, 1.0f / 60.0f);
        lag = std::max(lag, 1.0f - ik::nodePosition(pose, 4).y);  // cuanto baja la punta (modelo)
    }
    std::printf("    (la punta se queda atras: %.3f m)\n", lag);
    check(lag > 0.05f, "al subir de golpe, la punta se queda atras");
    for (int f = 0; f < 180; ++f) {
        reset();
        procedural::updateSprings(pose, world, stiff_chain, stiff, {}, 1.0f / 60.0f);
    }
    check(core::length(ik::nodePosition(pose, 4) - Vec3{0.6f, 1.0f, 0.0f}) < 0.01f, "quieto, vuelve a su pose");

    // Choca con una esfera en su camino.
    procedural::SpringChain hit_chain = procedural::makeSpringChain(nodes, 1);
    const std::vector<procedural::Sphere> ball = {{Vec3{0.45f, 0.75f, 0.0f}, 0.15f}};
    for (int f = 0; f < 180; ++f) {
        reset();
        procedural::updateSprings(pose, Mat4::identity(), hit_chain, loose, ball, 1.0f / 60.0f);
    }
    bool outside = true;
    for (int i = 2; i <= 4; ++i) {
        outside = outside && core::length(ik::nodePosition(pose, i) - ball[0].center) >= 0.15f + loose.radius - 0.005f;
    }
    check(outside, "no atraviesa la esfera");
}

void testLegs() {
    std::printf("Patas procedurales\n");
    // Cuerpo a 0.5 m con 4 patas (cadera, rodilla, pie), el suelo en y = 0.
    std::vector<asset::Node> nodes = {{"root", -1, Mat4::identity()}, {"body", 0, core::translate(Vec3{0.0f, 0.5f, 0.0f})}};
    std::vector<int> feet;
    const float sx[4] = {0.2f, -0.2f, 0.2f, -0.2f};
    const float sz[4] = {0.25f, 0.25f, -0.25f, -0.25f};
    for (int l = 0; l < 4; ++l) {
        nodes.push_back({"hip" + std::to_string(l), 1, core::translate(Vec3{sx[l], 0.0f, sz[l]})});
        const int hip = static_cast<int>(nodes.size()) - 1;
        nodes.push_back({"knee" + std::to_string(l), hip, core::translate(Vec3{sx[l] * 0.8f, 0.15f, 0.0f})});
        const int knee = static_cast<int>(nodes.size()) - 1;
        nodes.push_back({"foot" + std::to_string(l), knee, core::translate(Vec3{sx[l] * 0.3f, -0.65f, 0.0f})});
        feet.push_back(static_cast<int>(nodes.size()) - 1);
    }
    std::vector<Mat4> local(nodes.size());
    std::vector<Mat4> global(nodes.size());
    ik::Pose pose{&nodes, &local, &global};
    std::vector<procedural::Leg> legs;
    // Diagonales: 0 y 3 juntas, 1 y 2 juntas.
    const int groups[4] = {0, 1, 1, 0};
    std::vector<int> uppers;
    for (int l = 0; l < 4; ++l) {
        procedural::Leg leg;
        leg.end = feet[static_cast<std::size_t>(l)];
        leg.mid = nodes[static_cast<std::size_t>(leg.end)].parent;
        leg.upper = nodes[static_cast<std::size_t>(leg.mid)].parent;
        leg.group = groups[l];
        uppers.push_back(leg.upper);
        legs.push_back(leg);
    }
    const int body = procedural::commonAncestor(nodes, uppers);
    check(body == 1, "el cuerpo es el ancestro comun de las patas");
    float ground_y = 0.0f;
    const procedural::GroundQuery ground = [&](const Vec3& o, const Vec3& d, float max, Vec3& p, Vec3& n) {
        if (d.y >= 0.0f || o.y < ground_y) return false;
        const float t = (o.y - ground_y) / -d.y;
        if (t > max) return false;
        p = o + d * t;
        n = Vec3{0.0f, 1.0f, 0.0f};
        return true;
    };
    procedural::LegSettings settings;
    int steps = 0;
    bool slid = false;
    bool together = false;
    float worst_reach = 0.0f;
    std::vector<Vec3> last_planted(4);
    std::vector<bool> was_planted(4, false);
    const float speed = 1.0f;  // m/s hacia +Z
    for (int f = 0; f < 240; ++f) {
        const float z = speed * static_cast<float>(f) / 60.0f;
        const Mat4 world = core::translate(Vec3{0.0f, 0.0f, z});
        for (std::size_t i = 0; i < nodes.size(); ++i) local[i] = nodes[i].local;
        ik::recomputeGlobals(pose);
        steps += procedural::updateLegs(pose, world, legs, body, settings, ground, Vec3{0.0f, 0.0f, speed}, 1.0f / 60.0f);
        bool g0 = false;
        bool g1 = false;
        for (std::size_t l = 0; l < 4; ++l) {
            const procedural::Leg& leg = legs[l];
            if (leg.stepping) (leg.group == 0 ? g0 : g1) = true;
            if (!leg.stepping) {
                if (was_planted[l] && core::length(leg.planted - last_planted[l]) > 1e-4f) slid = true;
                // El pie de verdad esta donde dice (la pata llega).
                const Vec3 foot = ecs::transformPoint(world, ik::nodePosition(pose, leg.end));
                worst_reach = std::max(worst_reach, core::length(foot - leg.planted));
            }
            was_planted[l] = !leg.stepping;
            last_planted[l] = leg.planted;
        }
        together = together || (g0 && g1);
    }
    std::printf("    (%d pasos en 4 s; el pie apoyado se separa como mucho %.4f m de su sitio)\n", steps, worst_reach);
    check(steps >= 8, "al andar, las patas dan pasos");
    check(!slid, "un pie apoyado no resbala");
    check(worst_reach < 0.01f, "la pata llega a donde esta el pie");
    check(!together, "los dos grupos se turnan");

    // Suelo 0.2 m mas alto: el cuerpo sube con los pies.
    ground_y = 0.2f;
    for (int f = 0; f < 120; ++f) {
        const Mat4 world = core::translate(Vec3{0.0f, 0.0f, 4.0f + static_cast<float>(f) / 60.0f});
        for (std::size_t i = 0; i < nodes.size(); ++i) local[i] = nodes[i].local;
        ik::recomputeGlobals(pose);
        procedural::updateLegs(pose, world, legs, body, settings, ground, Vec3{0.0f, 0.0f, speed}, 1.0f / 60.0f);
    }
    const float body_y = ik::nodePosition(pose, body).y;
    std::printf("    (cuerpo a %.3f m con el suelo a 0.2)\n", body_y);
    check(std::abs(body_y - 0.7f) < 0.03f, "en suelo mas alto, el cuerpo sube");
}

// Diagnostico: --dump <modelo.crdata> lista los huesos de cada pieza y los
// humanos que se detectan.
int dumpModel(const char* file) {
    const auto model = assets::AssetManager::readModel(Uuid::generate(), file, "modelo");
    if (!model) {
        std::printf("no se pudo leer %s\n", file);
        return 1;
    }
    for (std::size_t p = 0; p < model->parts.size(); ++p) {
        const asset::ModelData& part = *model->parts[p];
        if (part.bones.empty()) continue;
        std::printf("pieza %zu: %zu nodos, %zu huesos, %zu clips\n", p, part.nodes.size(), part.bones.size(),
                    part.animations.size());
        const humanoid::Map map = humanoid::detect(part.nodes);
        for (std::size_t i = 0; i < part.nodes.size(); ++i) {
            const char* human = "";
            for (int b = 0; b < humanoid::kBoneCount; ++b) {
                if (map.nodes[static_cast<std::size_t>(b)] == static_cast<int>(i)) human = humanoid::boneName(static_cast<humanoid::Bone>(b));
            }
            std::printf("  %3zu  padre %3d  %-40s %s\n", i, part.nodes[i].parent, part.nodes[i].name.c_str(), human);
        }
        std::printf("humanoide: %s\n", map.valid ? "si" : "no");
    }
    return 0;
}


// Perro sintetico (Y arriba, mirando a +Z, la izquierda en +X): cuerpo,
// columna, pecho, cuello de 2, cabeza con mandibula y oreja, 4 patas de 3
// huesos + pie + dedo, y cola de 3.
std::vector<asset::Node> makeDog(std::vector<Mat4>* rest_out = nullptr) {
    struct Joint {
        const char* name;
        const char* parent;
        Vec3 p;
    };
    const std::vector<Joint> joints = {
        {"Body", "", {0.0f, 1.0f, -0.5f}},       {"Spine1", "Body", {0.0f, 1.05f, -0.1f}},
        {"Chest", "Spine1", {0.0f, 1.1f, 0.4f}}, {"Neck1", "Chest", {0.0f, 1.35f, 0.6f}},
        {"Neck2", "Neck1", {0.0f, 1.6f, 0.75f}}, {"Head", "Neck2", {0.0f, 1.75f, 0.95f}},
        {"Jaw", "Head", {0.0f, 1.65f, 1.1f}},    {"Ear_L", "Head", {0.08f, 1.9f, 0.9f}},
        {"FrontLeg_L_1", "Chest", {0.2f, 1.0f, 0.45f}},  {"FrontLeg_L_2", "FrontLeg_L_1", {0.2f, 0.6f, 0.5f}},
        {"FrontLeg_L_3", "FrontLeg_L_2", {0.2f, 0.3f, 0.45f}}, {"FrontFoot_L", "FrontLeg_L_3", {0.2f, 0.08f, 0.5f}},
        {"FrontToe_L", "FrontFoot_L", {0.2f, 0.0f, 0.6f}},
        {"FrontLeg_R_1", "Chest", {-0.2f, 1.0f, 0.45f}}, {"FrontLeg_R_2", "FrontLeg_R_1", {-0.2f, 0.6f, 0.5f}},
        {"FrontLeg_R_3", "FrontLeg_R_2", {-0.2f, 0.3f, 0.45f}}, {"FrontFoot_R", "FrontLeg_R_3", {-0.2f, 0.08f, 0.5f}},
        {"FrontToe_R", "FrontFoot_R", {-0.2f, 0.0f, 0.6f}},
        {"HindLeg_L_1", "Body", {0.2f, 0.95f, -0.55f}},  {"HindLeg_L_2", "HindLeg_L_1", {0.2f, 0.6f, -0.4f}},
        {"HindLeg_L_3", "HindLeg_L_2", {0.2f, 0.3f, -0.6f}}, {"HindFoot_L", "HindLeg_L_3", {0.2f, 0.08f, -0.55f}},
        {"HindToe_L", "HindFoot_L", {0.2f, 0.0f, -0.45f}},
        {"HindLeg_R_1", "Body", {-0.2f, 0.95f, -0.55f}}, {"HindLeg_R_2", "HindLeg_R_1", {-0.2f, 0.6f, -0.4f}},
        {"HindLeg_R_3", "HindLeg_R_2", {-0.2f, 0.3f, -0.6f}}, {"HindFoot_R", "HindLeg_R_3", {-0.2f, 0.08f, -0.55f}},
        {"HindToe_R", "HindFoot_R", {-0.2f, 0.0f, -0.45f}},
        {"Tail1", "Body", {0.0f, 1.05f, -0.8f}}, {"Tail2", "Tail1", {0.0f, 0.95f, -1.1f}}, {"Tail3", "Tail2", {0.0f, 0.8f, -1.35f}},
    };
    std::vector<asset::Node> nodes{{"Armature", -1, Mat4::identity()}};
    std::vector<Mat4> globals{Mat4::identity()};
    std::map<std::string, int> index;
    for (const Joint& j : joints) {
        const int parent = std::string(j.parent).empty() ? 0 : index.at(j.parent);
        const Mat4 global = core::translate(j.p);
        nodes.push_back(asset::Node{j.name, parent, core::inverse(globals[static_cast<std::size_t>(parent)]) * global});
        globals.push_back(global);
        index[j.name] = static_cast<int>(nodes.size() - 1);
    }
    if (rest_out != nullptr) *rest_out = globals;
    return nodes;
}

void testChainAndCreature() {
    std::printf("Cadenas largas (FABRIK) y animales\n");
    // Cadena de 4 huesos de 1 m hacia arriba (un cuello, un tentaculo).
    std::vector<asset::Node> nodes = {{"root", -1, Mat4::identity()}, {"a", 0, Mat4::identity()}};
    for (int i = 0; i < 4; ++i) nodes.push_back(asset::Node{"b" + std::to_string(i), static_cast<int>(nodes.size() - 1),
                                                            core::translate(Vec3{0.0f, 1.0f, 0.0f})});
    std::vector<Mat4> local;
    for (const asset::Node& n : nodes) local.push_back(n.local);
    std::vector<Mat4> global(nodes.size());
    ik::Pose pose{&nodes, &local, &global};
    ik::recomputeGlobals(pose);
    const std::vector<int> joints = ik::chainTo(nodes, 5, 4);
    check(joints.size() == 5 && joints.front() == 1 && joints.back() == 5, "chainTo: la cadena de 4 huesos hasta el extremo");
    const Vec3 target{1.5f, 2.2f, 1.0f};
    const Vec3 pole{3.0f, 1.0f, 0.0f};
    check(ik::chain(pose, joints, target, &pole, 1.0f), "resuelve una cadena de 4 huesos");
    const float error = core::length(ik::nodePosition(pose, 5) - target);
    std::printf("    (error: %.5f m)\n", error);
    bool lengths = true;
    for (std::size_t i = 0; i + 1 < joints.size(); ++i) {
        lengths = lengths && std::abs(core::length(ik::nodePosition(pose, joints[i + 1]) - ik::nodePosition(pose, joints[i])) - 1.0f) < 1e-3f;
    }
    check(error < 5e-3f && lengths, "el extremo llega sin estirar los huesos");
    check(ik::nodePosition(pose, 3).x > 0.2f, "con pole: se dobla hacia el");
    ik::chain(pose, joints, Vec3{0.0f, 10.0f, 10.0f}, nullptr, 1.0f);
    const Vec3 dir = core::normalize(ik::nodePosition(pose, 5) - ik::nodePosition(pose, 1));
    check(core::dot(dir, core::normalize(Vec3{0.0f, 10.0f, 10.0f} - ik::nodePosition(pose, 1))) > 0.999f,
          "si no llega, se estira hacia el objetivo");

    // Perro: patas, cabeza, cuello, cola y cuerpo.
    std::vector<Mat4> rest;
    const std::vector<asset::Node> dog = makeDog(&rest);
    const creature::Rig rig = creature::detect(dog, rest);
    int front = 0;
    int left = 0;
    bool three = true;
    bool feet = true;
    for (const creature::Leg& l : rig.legs) {
        front += l.front ? 1 : 0;
        left += l.left ? 1 : 0;
        three = three && l.bones == 3;
        const std::string n = dog[static_cast<std::size_t>(l.foot)].name;
        feet = feet && n.find("Foot") != std::string::npos;
        std::printf("    pata: %s (%d huesos, %s, %s)\n", n.c_str(), l.bones, l.front ? "delante" : "detras",
                    l.left ? "izquierda" : "derecha");
    }
    check(rig.valid && rig.legs.size() == 4 && front == 2 && left == 2, "encuentra 4 patas: 2 delante y 2 a la izquierda");
    check(three && feet, "el pie es el hueso que toca el suelo y la pata dobla 3 huesos");
    check(rig.head >= 0 && dog[static_cast<std::size_t>(rig.head)].name == "Head", "la cabeza");
    check(rig.neck.size() == 2 && dog[static_cast<std::size_t>(rig.neck.back())].name == "Neck2", "el cuello (2 huesos)");
    check(rig.tail.size() == 3, "la cola (3 huesos)");
    check(rig.body >= 0 && dog[static_cast<std::size_t>(rig.body)].name == "Body", "el cuerpo donde se unen las patas");
    check(rig.forward.z > 0.99f, "delante = hacia la cabeza");

    const std::vector<creature::RagdollBone> bones = creature::ragdollBones(dog, rest);
    float mass = 0.0f;
    bool sane = !bones.empty() && bones.front().parent < 0;
    for (const creature::RagdollBone& b : bones) {
        mass += b.mass;
        sane = sane && b.length > 0.0f && b.radius > 0.0f;
    }
    std::printf("    ragdoll del perro: %zu huesos\n", bones.size());
    check(bones.size() >= 12 && sane && std::abs(mass - 1.0f) < 1e-3f, "ragdoll del perro: cuerpo, cuello, cabeza, patas y cola");
    const std::vector<asset::Node> human = makeSkeleton(kMixamo, 0.0f, 1.0f, Mat4::identity(), false);
    const std::vector<creature::RagdollBone> human_bones = creature::ragdollBones(human, humanoid::restGlobals(human));
    check(human_bones.size() == 12, "ragdoll del humanoide: los 12 huesos de siempre");
}

void testPhysBones() {
    std::printf("Phys Bones\n");
    // Una cola horizontal de 4 huesos de 0.25 m (hacia +X) colgando de la raiz.
    std::vector<asset::Node> nodes = {{"root", -1, Mat4::identity()}, {"tail0", 0, core::translate(Vec3{0.0f, 2.0f, 0.0f})}};
    for (int i = 1; i <= 4; ++i) nodes.push_back(asset::Node{"tail" + std::to_string(i), i, core::translate(Vec3{0.25f, 0.0f, 0.0f})});
    const auto run = [&](const physbone::Settings& s, const std::vector<physbone::Collider>& colliders, float seconds) {
        std::vector<Mat4> local;
        for (const asset::Node& n : nodes) local.push_back(n.local);
        std::vector<Mat4> global(nodes.size());
        ik::Pose pose{&nodes, &local, &global};
        physbone::Chain chain = physbone::makeChain(nodes, 1, {}, s.end_length > 0.0f);
        for (int f = 0; f < static_cast<int>(seconds * 60.0f); ++f) {
            for (std::size_t i = 0; i < nodes.size(); ++i) local[i] = nodes[i].local;
            ik::recomputeGlobals(pose);
            physbone::update(pose, Mat4::identity(), chain, s, colliders, 1.0f / 60.0f);
        }
        return std::make_pair(chain, global);
    };
    physbone::Settings heavy;
    heavy.pull = 0.0f;
    heavy.stiffness = 0.0f;
    heavy.gravity = 1.0f;
    heavy.spring = 0.1f;
    auto [hanging, pose_hanging] = run(heavy, {}, 3.0f);
    const Vec3 tip = hanging.position.back();
    std::printf("    punta: (%.2f, %.2f)\n", tip.x, tip.y);
    check(tip.y < 1.1f, "con gravedad y sin pull cuelga hacia abajo");
    bool lengths = true;
    for (std::size_t i = 1; i < hanging.position.size(); ++i) {
        lengths = lengths && std::abs(core::length(hanging.position[i] - hanging.position[static_cast<std::size_t>(hanging.parent[i])]) - 0.25f) < 1e-3f;
    }
    check(lengths, "no se estira");
    check(pose_hanging[5].m[3][1] < 1.2f, "los huesos del esqueleto siguen a las particulas");

    physbone::Settings stiff = heavy;
    stiff.max_angle = 20.0f;
    auto [limited, pose_limited] = run(stiff, {}, 3.0f);
    const Vec3 first = core::normalize(limited.position[1] - limited.position[0]);
    const float angle = std::acos(std::clamp(core::dot(first, Vec3{1.0f, 0.0f, 0.0f}), -1.0f, 1.0f)) * 180.0f / core::kPi;
    std::printf("    angulo del primer hueso: %.1f grados\n", angle);
    check(angle <= 20.5f, "el angulo maximo limita cuanto cae");

    physbone::Collider ball;
    ball.shape = physbone::Collider::Shape::Sphere;
    ball.a = Vec3{0.5f, 1.6f, 0.0f};
    ball.radius = 0.3f;
    heavy.radius = 0.02f;
    auto [blocked, pose_blocked] = run(heavy, {ball}, 3.0f);
    bool outside = true;
    for (const Vec3& p : blocked.position) outside = outside && core::length(p - ball.a) >= 0.3f + 0.02f - 1e-3f;
    check(outside, "no atraviesa un collider (esfera)");
    (void)pose_limited;
    (void)pose_blocked;
}

void testRagdollPhysics() {
    std::printf("Ragdoll (Jolt)\n");
    ecs::World world;
    ecs::Entity ground = world.create("Suelo");
    ground.setLocalPosition(Vec3{0.0f, -0.5f, 0.0f});
    ground.add<physics::BoxCollider>().size = Vec3{20.0f, 1.0f, 20.0f};
    ecs::Entity dog = world.create("Perro");
    ecs::Ragdoll& rag = dog.add<ecs::Ragdoll>();
    rag.active = true;
    rag.runtime.ptr = std::make_shared<ecs::RagdollRuntime>();
    ecs::RagdollRuntime& rt = *rag.runtime.ptr;
    // Tres huesos en fila a 1.5 m de alto (una cola, una serpiente).
    for (int i = 0; i < 3; ++i) {
        ecs::RagdollRuntime::Bone b;
        b.name = "b" + std::to_string(i);
        b.node = i;
        b.parent = i - 1;
        b.radius = 0.08f;
        b.mass = 5.0f;
        b.swing = 30.0f;
        b.twist = 10.0f;
        b.world = b.previous = core::translate(Vec3{0.0f, 1.5f, 0.5f * static_cast<float>(i)});
        b.tip = Vec3{0.0f, 1.5f, 0.5f * static_cast<float>(i) + 0.5f};
        rt.bones.push_back(b);
    }
    rt.pose_ready = true;
    physics::PhysicsSystem physics;
    physics.start(world);
    for (int f = 0; f < 180; ++f) physics.update(world, 1.0f / 60.0f);
    check(rt.simulating && rt.sim_ready, "crea los cuerpos al activarlo");
    float lowest = 1e9f;
    float highest = -1e9f;
    for (const ecs::RagdollRuntime::Bone& b : rt.bones) {
        lowest = std::min(lowest, b.sim_position.y);
        highest = std::max(highest, b.sim_position.y);
    }
    std::printf("    alturas: %.2f .. %.2f\n", lowest, highest);
    check(highest < 0.4f && lowest > -0.05f, "cae y se queda sobre el suelo");
    float gap = 0.0f;
    for (std::size_t k = 1; k < rt.bones.size(); ++k) {
        const ecs::RagdollRuntime::Bone& p = rt.bones[k - 1];
        const Vec3 parent_tip = p.sim_position + ecs::quatRotate(p.sim_rotation, Vec3{0.0f, 0.0f, 0.5f});
        gap = std::max(gap, core::length(parent_tip - rt.bones[k].sim_position));
    }
    std::printf("    separacion en las articulaciones: %.3f m\n", gap);
    check(gap < 0.05f, "las articulaciones mantienen unidos los huesos");
    check(std::abs(dog.worldPosition().z - rt.bones.front().sim_position.z) < 1e-3f, "la entidad va con el cuerpo");
    rag.runtime.ptr->pushes.push_back(ecs::RagdollRuntime::Push{0, Vec3{0.0f, 60.0f, 0.0f}, {}, false});
    physics.update(world, 1.0f / 60.0f);
    physics.update(world, 1.0f / 60.0f);
    check(rt.bones.front().sim_position.y > lowest + 0.01f, "un empujon lo mueve");
    rag.active = false;
    physics.update(world, 1.0f / 60.0f);
    check(!rt.simulating && rt.blend_out > 0.0f, "al apagarlo quita los cuerpos y vuelve a la animacion mezclando");
    physics.stop();
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--dump") return dumpModel(argv[2]);
    testDetection();
    testRetarget();
    testIK();
    testSprings();
    testLegs();
    testChainAndCreature();
    testPhysBones();
    testRagdollPhysics();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
