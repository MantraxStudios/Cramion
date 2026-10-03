// Humanoides (deteccion y retargeting) y cinematica inversa.

#include "CramionCore/anim/FootPlacement.h"
#include "CramionCore/anim/Inertialization.h"
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
#include <functional>
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
// Motifect (y algunos BVH): "Leg" es el muslo y "Shin" la espinilla.
const std::map<std::string, std::string> kMotifect = {
    {"Hips", "Hips"},                 {"Spine", "Spine1"},             {"Chest", "Chest"},
    {"Neck", "Neck1"},                {"Head", "Head"},                {"LeftShoulder", "LeftShoulder"},
    {"LeftUpperArm", "LeftArm"},      {"LeftLowerArm", "LeftForeArm"}, {"LeftHand", "LeftHand"},
    {"RightShoulder", "RightShoulder"}, {"RightUpperArm", "RightArm"}, {"RightLowerArm", "RightForeArm"},
    {"RightHand", "RightHand"},       {"LeftUpperLeg", "LeftLeg"},     {"LeftLowerLeg", "LeftShin"},
    {"LeftFoot", "LeftFoot"},         {"LeftToes", "LeftToeBase"},     {"RightUpperLeg", "RightLeg"},
    {"RightLowerLeg", "RightShin"},   {"RightFoot", "RightFoot"},      {"RightToes", "RightToeBase"}};

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
    } rigs[] = {{"Mixamo", &kMixamo}, {"Unreal", &kUnreal}, {"Blender", &kBlender}, {"3ds Max Biped", &kBiped}, {"Motifect", &kMotifect}};
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

    // Reposo que no pisa el suelo (BVH de Motifect: cadera en el origen, pies a
    // -1 m) y un clip que si lo pisa: el destino no flota.
    asset::ModelData bvh;
    bvh.nodes = makeSkeleton(kMotifect, 0.0f, 1.0f, core::translate(Vec3{0.0f, -1.0f, 0.0f}), false);
    const int bvh_hips = nodeNamed(bvh.nodes, "Hips");
    asset::AnimationClip stand;
    stand.name = "DePie";
    stand.duration = 1.0f;
    asset::AnimationChannel stand_hips;
    stand_hips.node = bvh_hips;
    // la raiz baja 1 m: 1.97 en local = 0.97 sobre el suelo (agachado 3 cm)
    stand_hips.positions = {{0.0f, Vec3{0.0f, 1.97f, 0.0f}}, {1.0f, Vec3{0.0f, 1.97f, 0.0f}}};
    stand.channels = {stand_hips};
    asset::ModelData mixamo;
    mixamo.nodes = makeSkeleton(kMixamo, 0.0f, 1.0f, Mat4::identity(), false);
    asset::AnimationClip stand_converted;
    check(humanoid::retargetClip(bvh.nodes, stand, mixamo.nodes, stand_converted, &error), "clip de un BVH con la cadera en el origen");
    mixamo.animations = {stand_converted};
    const std::vector<Mat4> stood = poseAt(mixamo, 0.5f);
    const float hips_y = positionOf(stood[static_cast<std::size_t>(nodeNamed(mixamo.nodes, "mixamorig:Hips"))]).y;
    std::printf("    (cadera del destino a %.3f; en reposo 1.0)\n", hips_y);
    check(std::abs(hips_y - 0.97f) < 0.02f, "el destino no flota: la cadera queda a la altura del clip");
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

    // Un poco fuera de alcance (110 %): se estira hacia el, con IK blando (no
    // llega a bloquearse recto: se queda un pelo antes del largo total).
    const Vec3 near_target{0.0f, 2.2f, 0.0f};
    ik::twoBone(pose, 1, 2, 3, near_target, nullptr, 1.0f);
    const Vec3 hand = ik::nodePosition(pose, 3) - a;
    const float total = l1 + l2;
    std::printf("    (mano a %.3f de %.3f m)\n", core::length(hand), total);
    check(core::dot(core::normalize(hand), Vec3{0, 1, 0}) > 0.999f, "fuera de alcance, el brazo se estira hacia el objetivo");
    check(core::length(hand) > total * 0.95f && core::length(hand) < total * 0.999f, "IK blando: casi recto, sin bloquearse");

    // Muy lejos (2.5 veces el brazo): no lo persigue (antes se estiraba de golpe).
    const std::vector<Mat4> antes = global;
    ik::twoBone(pose, 1, 2, 3, Vec3{0.0f, 5.0f, 0.0f}, nullptr, 1.0f);
    check(core::length(positionOf(global[3]) - positionOf(antes[3])) < 1e-4f, "un objetivo muy lejos no estira el brazo");

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

// --- Pies al suelo (anim/FootPlacement.h) -----------------------------------

namespace {

// Escalera de prueba hacia +Z: 5 escalones de 0.17 x 0.3 m desde z = 2, un
// rellano de 1.2 m y 5 de bajada.
float stairsHeight(float z) {
    constexpr float kH = 0.17f;
    constexpr float kD = 0.3f;
    if (z < 2.0f) return 0.0f;
    if (z < 2.0f + 5.0f * kD) return (std::floor((z - 2.0f) / kD) + 1.0f) * kH;
    const float down = 2.0f + 5.0f * kD + 1.2f;
    if (z < down) return 5.0f * kH;
    if (z < down + 5.0f * kD) return (4.0f - std::floor((z - down) / kD)) * kH;
    return 0.0f;
}

// Rayo hacia abajo contra un suelo de alturas (como la fisica del IK: si
// empieza dentro de algo, no cuenta).
ik::GroundRay heightRay(const std::function<float(float)>& height) {
    return [height](const Vec3& o, const Vec3& d, float max, Vec3& p, Vec3& n) {
        if (d.y >= 0.0f) return false;
        const float h = height(o.z);
        if (o.y < h || o.y - h > max) return false;
        p = Vec3{o.x, h, o.z};
        n = Vec3{0.0f, 1.0f, 0.0f};
        return true;
    };
}

// Andar sintetico (la "animacion" en el sitio, como un clip de Mixamo): la
// cadera algo mas baja que en reposo y cada tobillo en su punto del ciclo:
// apoyado va hacia atras a la velocidad del personaje (quieto en el mundo);
// en el aire, hacia delante en arco. Las piernas casi rectas en los extremos.
struct Gait {
    float speed = 1.2f;  // m/s
    float half = 0.25f;  // medio tramo apoyado (m)
    float lift = 0.12f;  // altura del pie en el aire
    float drop = 0.05f;  // la cadera, mas baja que en reposo
    float cycle() const { return 2.0f * half / (speed * 0.6f); }
};

void poseGait(const ik::Pose& pose, const std::vector<asset::Node>& nodes, const humanoid::Map& map,
              const std::vector<Mat4>& rest, float time, const Gait& g) {
    using humanoid::Bone;
    for (std::size_t i = 0; i < nodes.size(); ++i) (*pose.local)[i] = nodes[i].local;
    ik::recomputeGlobals(pose);
    ik::translateGlobal(pose, map[Bone::Hips], Vec3{0.0f, -g.drop, 0.0f});
    const Bone legs[2][3] = {{Bone::LeftUpperLeg, Bone::LeftLowerLeg, Bone::LeftFoot},
                             {Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot}};
    for (int side = 0; side < 2; ++side) {
        float phase = time / g.cycle() + (side == 0 ? 0.0f : 0.5f);
        phase -= std::floor(phase);
        float z = 0.0f;
        float y = 0.08f;
        if (phase < 0.6f) {
            z = g.half - 2.0f * g.half * (phase / 0.6f);
        } else {
            const float s = (phase - 0.6f) / 0.4f;
            z = -g.half + 2.0f * g.half * (s * s * (3.0f - 2.0f * s));
            y += g.lift * std::sin(core::kPi * s);
        }
        const int foot = map[legs[side][2]];
        const Vec3 pole = ik::nodePosition(pose, map[legs[side][1]]) + Vec3{0.0f, 0.0f, 1.0f};
        ik::twoBone(pose, map[legs[side][0]], map[legs[side][1]], foot, Vec3{side == 0 ? 0.1f : -0.1f, y, z}, &pole, 1.0f);
        Vec3 t{};
        Quat r{};
        Vec3 s{};
        ecs::decomposeMatrix(rest[static_cast<std::size_t>(foot)], t, r, s);
        ik::setGlobalRotation(pose, foot, r);  // el pie plano
    }
}

// Pierna de 3 articulaciones (cadera, rodilla, tobillo) en el modelo.
struct Leg3 {
    std::vector<asset::Node> nodes;
    std::vector<Mat4> local;
    std::vector<Mat4> global;
    ik::Pose pose;
    explicit Leg3(const std::vector<Vec3>& joints) {
        nodes.push_back({"root", -1, Mat4::identity()});
        Vec3 parent{};
        for (std::size_t i = 0; i < joints.size(); ++i) {
            nodes.push_back({"j" + std::to_string(i), static_cast<int>(i), core::translate(joints[i] - parent)});
            parent = joints[i];
        }
        for (const asset::Node& n : nodes) local.push_back(n.local);
        global.resize(nodes.size());
        pose = ik::Pose{&nodes, &local, &global};
        ik::recomputeGlobals(pose);
    }
};

// Lo que se aparta `p` de la recta a-b.
Vec3 offLine(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 axis = b - a;
    const Vec3 rel = p - a;
    return rel - axis * (core::dot(rel, axis) / core::dot(axis, axis));
}

}  // namespace

void testFootPlacement() {
    std::printf("Pies al suelo (andar, escaleras, saltar)\n");
    using humanoid::Bone;
    const Vec3 forward{0.0f, 0.0f, 1.0f};

    // --- twoBone: sin encoger la pierna ni doblar la rodilla hacia atras ---
    {
        // Pierna casi recta (99.9 %), el objetivo en su propio tobillo: antes
        // el IK blando la encogia y el pie subia ~2.5 cm (pies flotando al andar).
        Leg3 leg({{0.1f, 0.95f, 0.0f}, {0.1f, 0.52f, 0.02f}, {0.1f, 0.08f, 0.0f}});
        const Vec3 ankle = ik::nodePosition(leg.pose, 3);
        ik::twoBone(leg.pose, 1, 2, 3, ankle, nullptr, 1.0f, &forward);
        const float moved = core::length(ik::nodePosition(leg.pose, 3) - ankle);
        std::printf("    (el tobillo se mueve %.5f m)\n", moved);
        check(moved < 1e-4f, "objetivo en su propio pie con la pierna casi recta: el pie no sube");
    }
    for (const float knee_z : {0.0f, -0.012f}) {
        // Rodilla recta del todo o un pelo hiperextendida (hacia atras), y el
        // pie sube 25 cm (un escalon): la rodilla va hacia delante.
        Leg3 leg({{0.0f, 0.9f, 0.0f}, {0.0f, 0.45f, knee_z}, {0.0f, 0.0f, 0.0f}});
        const bool solved = ik::twoBone(leg.pose, 1, 2, 3, Vec3{0.0f, 0.25f, 0.0f}, nullptr, 1.0f, &forward);
        const float knee = ik::nodePosition(leg.pose, 2).z;
        const float reach = core::length(ik::nodePosition(leg.pose, 3) - Vec3{0.0f, 0.25f, 0.0f});
        std::printf("    (rodilla en z = %.3f, tobillo a %.4f m del objetivo)\n", knee, reach);
        check(solved && knee > 0.1f && reach < 1e-3f,
              knee_z == 0.0f ? "pierna recta del todo: la rodilla se dobla hacia delante"
                             : "rodilla hiperextendida: al subir el pie se dobla hacia delante, no hacia atras");
    }
    {
        // Pata de 3 huesos recta en la animacion; en reposo, la rodilla hacia
        // delante y el corvejon hacia atras: al subir el pie se dobla como en
        // reposo (FABRIK con una cadena recta no sabia hacia donde doblarla).
        Leg3 rest_leg({{0.0f, 1.0f, 0.0f}, {0.0f, 0.66f, 0.08f}, {0.0f, 0.33f, 0.0f}, {0.0f, 0.0f, 0.0f}});
        std::vector<Mat4> rest(rest_leg.global);
        Leg3 leg({{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f - 0.3493f, 0.0f}, {0.0f, 1.0f - 0.6889f, 0.0f}, {0.0f, 1.0f - 1.0189f, 0.0f}});
        const std::vector<int> joints{1, 2, 3, 4};
        const std::vector<Vec3> hints = ik::restBendHints(leg.pose, rest, joints);
        const Vec3 goal = ik::nodePosition(leg.pose, 4) + Vec3{0.0f, 0.2f, 0.0f};
        ik::chain(leg.pose, joints, goal, nullptr, 1.0f, 12, &hints);
        const Vec3 p1 = ik::nodePosition(leg.pose, 1);
        const Vec3 p2 = ik::nodePosition(leg.pose, 2);
        const Vec3 p3 = ik::nodePosition(leg.pose, 3);
        const Vec3 p4 = ik::nodePosition(leg.pose, 4);
        const float error = core::length(p4 - goal);
        std::printf("    (rodilla z = %.3f, corvejon z = %.3f, error %.4f m)\n", offLine(p2, p1, p3).z, offLine(p3, p2, p4).z, error);
        check(hints[1].z > 0.9f && hints[2].z < -0.9f, "pistas del reposo: rodilla hacia delante, corvejon hacia atras");
        check(error < 5e-3f && offLine(p2, p1, p3).z > 0.02f && offLine(p3, p2, p4).z < -0.02f,
              "pata recta al subir el pie: se dobla como en reposo y llega");
    }

    // --- Andar con el humanoide ---
    const std::vector<asset::Node> nodes = makeSkeleton(kMixamo, 0.0f, 1.0f, Mat4::identity(), false);
    const humanoid::Map map = humanoid::detect(nodes);
    const std::vector<Mat4> rest = humanoid::restGlobals(nodes);
    std::vector<Mat4> local(nodes.size());
    std::vector<Mat4> global(nodes.size());
    ik::Pose pose{&nodes, &local, &global};
    std::vector<ik::GroundLeg> legs(2);
    const Bone sides[2][4] = {{Bone::LeftUpperLeg, Bone::LeftLowerLeg, Bone::LeftFoot, Bone::LeftToes},
                              {Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot, Bone::RightToes}};
    for (int s = 0; s < 2; ++s) {
        legs[static_cast<std::size_t>(s)].joints = {map[sides[s][0]], map[sides[s][1]], map[sides[s][2]]};
        legs[static_cast<std::size_t>(s)].toe = map[sides[s][3]];
        legs[static_cast<std::size_t>(s)].bend_hints = {Vec3{}, forward, Vec3{}};
    }
    const int hips = map[Bone::Hips];
    ik::GroundSettings settings;
    settings.max_step = 0.45f;
    const Gait gait;
    struct Result {
        float stance = 0.0f;    // lo que se aparta del suelo un pie apoyado
        float below = 0.0f;     // lo que se mete en el suelo un tobillo o una punta
        float pelvis = 0.0f;    // el mayor salto de la cadera en un frame
        float knee = 0.0f;      // cuanto se dobla una rodilla hacia atras
        int stance_samples = 0;
    };
    // El personaje anda hacia +Z; su base (el Character Controller) va pegada
    // al suelo bajo su centro: sube y baja cada escalon de golpe en un frame.
    const auto walk = [&](const std::function<float(float)>& height, float seconds) {
        Result r;
        ik::GroundState state;
        const ik::GroundRay ray = heightRay(height);
        const float dt = 1.0f / 60.0f;
        float last_pelvis = 0.0f;
        Vec3 last_animated[2]{};
        int quiet_frames[2] = {0, 0};  // frames seguidos apoyado y quieto (se mide tras asentarse 0.1 s)
        const int frames = static_cast<int>(seconds * 60.0f);
        for (int f = 0; f < frames; ++f) {
            const float t = static_cast<float>(f) * dt;
            const float z = gait.speed * t;
            const Mat4 world = core::translate(Vec3{0.0f, height(z), z});
            poseGait(pose, nodes, map, rest, t, gait);
            Vec3 animated[2];
            for (std::size_t s = 0; s < 2; ++s) {
                animated[s] = ecs::transformPoint(world, ik::nodePosition(pose, legs[s].joints[2]));
            }
            ik::groundLegs(pose, world, hips, legs, forward, settings, ray, state, dt);
            const float pelvis = ecs::transformPoint(world, ik::nodePosition(pose, hips)).y;
            if (f > 30) r.pelvis = std::max(r.pelvis, std::abs(pelvis - last_pelvis));
            last_pelvis = pelvis;
            for (std::size_t s = 0; s < 2; ++s) {
                const Vec3 moved = animated[s] - last_animated[s];
                const bool quiet = f > 0 && std::sqrt(moved.x * moved.x + moved.z * moved.z) / dt < 0.2f;
                quiet_frames[s] = state.feet[s].plant > 0.98f && quiet ? quiet_frames[s] + 1 : 0;
                last_animated[s] = animated[s];
            }
            if (f < 60) continue;  // se asienta
            for (std::size_t s = 0; s < 2; ++s) {
                const Vec3 a = ecs::transformPoint(world, ik::nodePosition(pose, legs[s].joints[0]));
                const Vec3 k = ecs::transformPoint(world, ik::nodePosition(pose, legs[s].joints[1]));
                const Vec3 ankle = ecs::transformPoint(world, ik::nodePosition(pose, legs[s].joints[2]));
                const Vec3 toe = ecs::transformPoint(world, ik::nodePosition(pose, legs[s].toe));
                const float support = std::max(height(ankle.z), height(toe.z));
                if (quiet_frames[s] > 6) {
                    // A su altura del clip sobre su suelo (ni flota ni se hunde).
                    r.stance = std::max(r.stance, std::abs(ankle.y - (support + state.feet[s].lift)));
                    ++r.stance_samples;
                }
                r.below = std::max({r.below, height(ankle.z) + 0.08f - ankle.y, height(toe.z) - toe.y});
                r.knee = std::max(r.knee, -core::dot(offLine(k, a, ankle), forward));
            }
        }
        return r;
    };
    const Result flat = walk([](float) { return 0.0f; }, 4.0f);
    std::printf("    (llano: pie apoyado a %.4f m de su sitio en %d muestras, cadera salta %.4f m)\n", flat.stance,
                flat.stance_samples, flat.pelvis);
    check(flat.stance_samples > 50 && flat.stance < 0.003f, "andando en llano el pie apoyado no flota (ni se hunde)");
    const Result stairs = walk(stairsHeight, 7.0f);
    std::printf("    (escalera: apoyado %.4f m, se mete %.4f m, cadera salta %.4f m/frame, rodilla atras %.4f m)\n",
                stairs.stance, stairs.below, stairs.pelvis, stairs.knee);
    check(stairs.stance_samples > 50 && stairs.stance < 0.015f, "en la escalera el pie apoyado pisa su escalon");
    check(stairs.below < 0.012f, "ni el tobillo ni la punta se meten en los escalones");
    check(stairs.pelvis < 0.03f, "subir o bajar un escalon de golpe (el controlador) no da saltos en la cadera");
    check(stairs.knee < 0.005f && flat.knee < 0.005f, "las rodillas nunca se doblan hacia atras");

    // --- En el aire: los pies se quedan como en la animacion ---
    {
        ik::GroundState state;
        const ik::GroundRay ray = heightRay([](float) { return 0.0f; });
        const Mat4 world = core::translate(Vec3{0.0f, 0.3f, 0.0f});  // saltando: 30 cm por encima del suelo
        float moved = 0.0f;
        for (int f = 0; f < 15; ++f) {
            poseGait(pose, nodes, map, rest, 0.1f, gait);
            const Vec3 before = ik::nodePosition(pose, legs[0].joints[2]);
            ik::groundLegs(pose, world, hips, legs, forward, settings, ray, state, 1.0f / 60.0f, ik::Support::Air);
            if (f > 10) moved = std::max(moved, core::length(ik::nodePosition(pose, legs[0].joints[2]) - before));
        }
        std::printf("    (en el aire el pie se mueve %.4f m)\n", moved);
        check(moved < 2e-3f, "en el aire (Character Controller) los pies no buscan el suelo");
    }
    {
        // Sin saber si salta: subir deprisa varios frames seguidos tambien es aire.
        ik::GroundState state;
        const ik::GroundRay ray = heightRay([](float) { return 0.0f; });
        for (int f = 0; f < 12; ++f) {
            const Mat4 world = core::translate(Vec3{0.0f, 4.5f * static_cast<float>(f) / 60.0f, 0.0f});
            poseGait(pose, nodes, map, rest, 0.1f, gait);
            ik::groundLegs(pose, world, hips, legs, forward, settings, ray, state, 1.0f / 60.0f);
        }
        std::printf("    (saltando, peso del IK %.3f)\n", state.active);
        check(state.active < 0.3f, "al saltar (subir deprisa) el IK se aparta solo");
    }

    // --- Un perro con las patas de delante en un escalon ---
    {
        std::vector<Mat4> dog_rest;
        const std::vector<asset::Node> dog = makeDog(&dog_rest);
        const creature::Rig rig = creature::detect(dog, dog_rest);
        std::vector<Mat4> dog_local(dog.size());
        std::vector<Mat4> dog_global(dog.size());
        ik::Pose dog_pose{&dog, &dog_local, &dog_global};
        const auto reset = [&] {
            for (std::size_t i = 0; i < dog.size(); ++i) dog_local[i] = dog[i].local;
            ik::recomputeGlobals(dog_pose);
        };
        reset();
        std::vector<ik::GroundLeg> dog_legs;
        for (const creature::Leg& l : rig.legs) {
            ik::GroundLeg leg;
            leg.joints = ik::chainTo(dog, l.foot, l.bones);
            leg.bend_hints = ik::restBendHints(dog_pose, dog_rest, leg.joints);
            dog_legs.push_back(leg);
        }
        ik::GroundSettings dog_settings;
        dog_settings.max_step = 0.4f;
        dog_settings.align_body = true;
        const auto run = [&](const std::function<float(float)>& height) {
            ik::GroundState state;
            const ik::GroundRay ray = heightRay(height);
            for (int f = 0; f < 40; ++f) {
                reset();
                ik::groundLegs(dog_pose, Mat4::identity(), rig.body, dog_legs, rig.forward, dog_settings, ray, state,
                               1.0f / 60.0f);
            }
        };
        run([](float) { return 0.0f; });
        float still = 0.0f;
        for (const ik::GroundLeg& leg : dog_legs) {
            still = std::max(still, core::length(ik::nodePosition(dog_pose, leg.joints.back()) -
                                                 positionOf(dog_rest[static_cast<std::size_t>(leg.joints.back())])));
        }
        check(still < 2e-3f, "perro en llano: las patas se quedan como estan");
        const Vec3 front_root_before = ik::nodePosition(dog_pose, dog_legs[0].joints.front());
        run([](float z) { return z > 0.2f ? 0.15f : 0.0f; });
        float worst = 0.0f;
        bool sides_kept = true;
        float front_root_rise = 0.0f;
        float hind_root_rise = 0.0f;
        for (std::size_t i = 0; i < dog_legs.size(); ++i) {
            const std::vector<int>& j = dog_legs[i].joints;
            const Vec3 paw = ik::nodePosition(dog_pose, j.back());
            const float wanted = (paw.z > 0.2f ? 0.15f : 0.0f) + 0.08f;
            worst = std::max(worst, std::abs(paw.y - wanted));
            for (std::size_t k = 1; k + 1 < j.size(); ++k) {
                const Vec3 off = offLine(ik::nodePosition(dog_pose, j[k]), ik::nodePosition(dog_pose, j[k - 1]),
                                         ik::nodePosition(dog_pose, j[k + 1]));
                const Vec3 off_rest = offLine(positionOf(dog_rest[static_cast<std::size_t>(j[k])]),
                                              positionOf(dog_rest[static_cast<std::size_t>(j[k - 1])]),
                                              positionOf(dog_rest[static_cast<std::size_t>(j[k + 1])]));
                if (core::length(off_rest) > 1e-3f) sides_kept = sides_kept && core::dot(off, off_rest) > 0.0f;
            }
            const float rise = ik::nodePosition(dog_pose, j.front()).y - positionOf(dog_rest[static_cast<std::size_t>(j.front())]).y;
            (paw.z > 0.2f ? front_root_rise : hind_root_rise) = rise;
        }
        (void)front_root_before;
        std::printf("    (perro en el escalon: patas a %.4f m de su sitio, hombros %+.3f, caderas %+.3f)\n", worst,
                    front_root_rise, hind_root_rise);
        check(worst < 0.02f, "perro: las patas de delante pisan el escalon y las de atras el suelo");
        check(front_root_rise - hind_root_rise > 0.05f, "perro: el cuerpo se inclina (morro arriba)");
        check(sides_kept, "perro: ninguna articulacion de las patas se dobla al reves");
    }
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


void testGroundTiltAndLook() {
    std::printf("Inclinar el cuerpo y mirar con el cuello\n");
    // Cuatro patas de un animal que mira a -Z: delante en z = -0.5, detras en z = +0.5.
    const std::vector<Vec3> feet = {{-0.2f, 0.0f, -0.5f}, {0.2f, 0.0f, -0.5f}, {-0.2f, 0.0f, 0.5f}, {0.2f, 0.0f, 0.5f}};
    const Vec3 forward{0.0f, 0.0f, -1.0f};
    const Vec3 right = core::normalize(core::cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
    const auto tilted = [&](const std::vector<float>& h, const Vec3& v) { return ecs::quatRotate(ik::groundTilt(feet, h, forward), v); };
    check(tilted({-0.2f, -0.2f, 0.0f, 0.0f}, forward).y < -0.1f, "el suelo baja por delante: el morro baja");
    check(tilted({0.2f, 0.2f, 0.0f, 0.0f}, forward).y > 0.1f, "el suelo sube por delante: el morro sube");
    // Derecha del animal: +X (mira a -Z). Sube por la derecha -> la derecha sube.
    check(tilted({0.0f, 0.2f, 0.0f, 0.2f}, right).y > 0.1f && right.x > 0.9f, "el suelo sube por la derecha: ese lado sube");
    check(std::abs(tilted({0.0f, 0.0f, 0.0f, 0.0f}, forward).y) < 1e-5f, "suelo plano: no se inclina");
    const Vec3 steep = tilted({-5.0f, -5.0f, 0.0f, 0.0f}, forward);
    check(std::asin(std::clamp(-steep.y, -1.0f, 1.0f)) * 180.0f / core::kPi <= 30.5f, "como mucho 30 grados");

    // Cuello de 3 huesos hacia arriba y la cabeza mirando a -Z; objetivo detras.
    std::vector<asset::Node> nodes = {{"root", -1, Mat4::identity()},
                                      {"neck1", 0, core::translate(Vec3{0.0f, 1.0f, 0.0f})},
                                      {"neck2", 1, core::translate(Vec3{0.0f, 0.2f, 0.0f})},
                                      {"head", 2, core::translate(Vec3{0.0f, 0.2f, 0.0f})}};
    std::vector<Mat4> local;
    for (const asset::Node& n : nodes) local.push_back(n.local);
    std::vector<Mat4> global(nodes.size());
    ik::Pose pose{&nodes, &local, &global};
    ik::recomputeGlobals(pose);
    ik::lookChain(pose, {1, 2, 3}, forward, Vec3{0.0f, 1.4f, 5.0f}, 1.0f, 70.0f);
    const Vec3 now = ecs::quatRotate(ik::nodeRotation(pose, 3), forward);
    const float turned = std::acos(std::clamp(core::dot(now, forward), -1.0f, 1.0f)) * 180.0f / core::kPi;
    std::printf("    la cabeza giro %.1f grados (objetivo detras)\n", turned);
    check(turned <= 70.5f && turned > 60.0f, "con el objetivo detras la cabeza gira el maximo y no mas");
    for (std::size_t i = 0; i < nodes.size(); ++i) local[i] = nodes[i].local;
    ik::recomputeGlobals(pose);
    ik::lookChain(pose, {1, 2, 3}, forward, Vec3{2.0f, 1.4f, -4.0f}, 1.0f, 70.0f);
    const Vec3 dir = core::normalize(Vec3{2.0f, 1.4f, -4.0f} - ik::nodePosition(pose, 3));
    check(core::dot(ecs::quatRotate(ik::nodeRotation(pose, 3), forward), dir) > 0.99f, "dentro del maximo, mira justo al objetivo");
    const Vec3 up_neck = ik::nodePosition(pose, 2) - ik::nodePosition(pose, 1);
    check(core::length(up_neck) > 0.199f && core::length(up_neck) < 0.201f, "el cuello no se estira");
}

// Blend Trees: pesos 1D y 2D, mezcla de poses, fundido y guardado.
void testBlendTrees() {
    std::printf("\nBlend Trees\n");
    using ecs::AnimatorMotion;
    ecs::AnimatorState walk;
    walk.motion = AnimatorMotion::BlendTree1D;
    walk.children.resize(3);
    walk.children[0].threshold = 0.0f;  // quieto
    walk.children[1].threshold = 2.0f;  // andar
    walk.children[2].threshold = 6.0f;  // correr
    auto w = ecs::blendTreeWeights(walk, 1.0f);
    check(std::abs(w[0] - 0.5f) < 1e-5f && std::abs(w[1] - 0.5f) < 1e-5f && w[2] == 0.0f, "1D: a medio camino entre quieto y andar");
    w = ecs::blendTreeWeights(walk, 5.0f);
    check(w[0] == 0.0f && std::abs(w[1] - 0.25f) < 1e-5f && std::abs(w[2] - 0.75f) < 1e-5f, "1D: entre andar y correr");
    w = ecs::blendTreeWeights(walk, -3.0f);
    check(w[0] == 1.0f, "1D: por debajo, el primero");
    w = ecs::blendTreeWeights(walk, 9.0f);
    check(w[2] == 1.0f, "1D: por encima, el ultimo");
    std::swap(walk.children[0], walk.children[2]);  // el orden de la lista no importa
    w = ecs::blendTreeWeights(walk, 5.0f);
    check(std::abs(w[0] - 0.75f) < 1e-5f && std::abs(w[1] - 0.25f) < 1e-5f, "1D: se ordenan por umbral");

    ecs::AnimatorState move;
    move.motion = AnimatorMotion::BlendTree2D;
    const core::Vec2 points[] = {{0, 0}, {0, 1}, {0, -1}, {1, 0}, {-1, 0}};
    for (const core::Vec2& pt : points) {
        ecs::BlendTreeChild child;
        child.position = pt;
        move.children.push_back(child);
    }
    w = ecs::blendTreeWeights(move, 0.0f, 1.0f);
    check(std::abs(w[1] - 1.0f) < 1e-4f, "2D: en un punto suena solo ese clip");
    w = ecs::blendTreeWeights(move, 0.0f, 0.0f);
    check(std::abs(w[0] - 1.0f) < 1e-4f, "2D: en el centro, quieto");
    w = ecs::blendTreeWeights(move, 0.5f, 0.5f);
    float sum = 0.0f;
    for (const float x : w) sum += x;
    check(std::abs(sum - 1.0f) < 1e-4f && w[1] > 0.1f && w[3] > 0.1f && std::abs(w[1] - w[3]) < 1e-4f && w[2] < 1e-4f && w[4] < 1e-4f,
          "2D: en diagonal mezcla adelante y derecha a partes iguales");

    // Mezcla de poses: un nodo que en un clip esta en x=0 y en el otro en x=2 girado 90 grados.
    asset::ModelData model;
    model.nodes.push_back({"raiz", -1, Mat4::identity()});
    model.nodes.push_back({"hueso", 0, Mat4::identity()});
    const float h = std::sqrt(0.5f);
    for (int k = 0; k < 2; ++k) {
        asset::AnimationClip clip;
        clip.name = k == 0 ? "a" : "b";
        clip.duration = 1.0f;
        asset::AnimationChannel channel;
        channel.node = 1;
        channel.positions.push_back({0.0f, Vec3{k == 0 ? 0.0f : 2.0f, 0.0f, 0.0f}});
        channel.rotations.push_back({0.0f, k == 0 ? Quat{} : Quat{0.0f, h, 0.0f, h}});
        clip.channels.push_back(channel);
        model.animations.push_back(clip);
    }
    anim::Animator animator(model);
    animator.evaluateBlend({{0, 0.0f, 0.5f}, {1, 0.0f, 0.5f}});
    const Mat4& local = animator.locals()[1];
    check(std::abs(local.m[3][0] - 1.0f) < 1e-4f, "mezcla 50/50: la posicion a medio camino");
    const float angle = std::atan2(-local.m[0][2], local.m[0][0]) * 57.2957795f;
    check(std::abs(std::abs(angle) - 45.0f) < 0.5f, "mezcla 50/50: el giro a medio camino (45 grados)");
    animator.evaluateBlend({{1, 0.0f, 3.0f}});
    check(std::abs(animator.locals()[1].m[3][0] - 2.0f) < 1e-4f, "un solo clip con cualquier peso = ese clip");
    animator.evaluateBlend({});
    check(std::abs(animator.locals()[1].m[3][0]) < 1e-6f, "sin muestras: pose de reposo");
    animator.evaluateBlend({{0, 0.0f, 0.25f}, {-1, 0.0f, 0.75f}});
    check(std::abs(animator.locals()[1].m[3][0]) < 1e-6f, "el reposo tambien se puede mezclar");

    // Transicion con fundido y guardado del Blend Tree.
    ecs::AnimatorController controller;
    controller.parameters.push_back({"velocidad", ecs::AnimatorParameterType::Float, 0.0f});
    controller.parameters.push_back({"saltar", ecs::AnimatorParameterType::Trigger, 0.0f});
    ecs::AnimatorState locomotion = walk;
    locomotion.name = "Moverse";
    locomotion.blend_parameter = "velocidad";
    locomotion.children[0].clip_name = "correr";
    controller.states.push_back(locomotion);
    ecs::AnimatorState jump;
    jump.name = "Saltar";
    jump.clip_name = "salto";
    controller.states.push_back(jump);
    ecs::AnimatorTransition t;
    t.from = 0;
    t.to = 1;
    t.duration = 0.2f;
    t.conditions.push_back({"saltar", ecs::AnimatorConditionMode::If, 0.0f});
    controller.transitions.push_back(t);
    ecs::AnimatorRuntime runtime;
    ecs::stepAnimatorController(controller, runtime, 1.0f);
    runtime.values["saltar"] = 1.0f;
    check(ecs::stepAnimatorController(controller, runtime, 1.0f) && runtime.state == 1 && runtime.last_transition == 0,
          "la maquina dice por que transicion cambio (para su fundido)");

    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_blend_test.cranimator";
    check(ecs::saveAnimatorController(controller, file), "guardar el controlador con Blend Tree");
    ecs::AnimatorController loaded;
    check(ecs::loadAnimatorController(file, loaded) && loaded.states.size() == 2 &&
              loaded.states[0].motion == AnimatorMotion::BlendTree1D && loaded.states[0].blend_parameter == "velocidad" &&
              loaded.states[0].children.size() == 3 && loaded.states[0].children[0].clip_name == "correr" &&
              std::abs(loaded.transitions[0].duration - 0.2f) < 1e-6f && !loaded.states[1].isBlendTree(),
          "cargarlo: el arbol, sus clips, umbrales y el fundido");
    std::filesystem::remove(file);
}

void testInertialization() {
    std::printf("\nInercializacion\n");
    using cramion::anim::Inertializer;
    // Un nodo que se movia a 2 m/s en X; la animacion nueva lo deja quieto en 0.
    Inertializer in;
    std::vector<Mat4> pose(1);
    const float dt = 1.0f / 60.0f;
    for (int f = 0; f < 3; ++f) {
        pose[0] = core::composeTrs(Vec3{1.0f + 2.0f * dt * static_cast<float>(f), 0, 0}, Quat{}, Vec3{1.0f});
        in.apply(pose, dt);
    }
    const float last_x = 1.0f + 4.0f * dt;
    std::vector<Mat4> fresh(1, core::composeTrs(Vec3{0, 0, 0}, Quat{}, Vec3{1.0f}));
    in.start(fresh, fresh, 0.3f, dt);
    check(in.active(), "empieza la transicion");
    std::vector<Mat4> out = fresh;
    in.apply(out, dt);
    const float first = out[0].m[3][0];
    check(std::abs(first - last_x) < 0.05f, "el primer frame sigue donde estaba (sin salto)");
    // Sin velocidad (quieto en el mismo punto) cae antes hacia la pose nueva.
    Inertializer still;
    std::vector<Mat4> rest(1, core::composeTrs(Vec3{last_x, 0, 0}, Quat{}, Vec3{1.0f}));
    still.apply(rest, dt);
    rest[0] = core::composeTrs(Vec3{last_x, 0, 0}, Quat{}, Vec3{1.0f});
    still.apply(rest, dt);
    still.start(fresh, fresh, 0.3f, dt);
    std::vector<Mat4> still_out = fresh;
    still.apply(still_out, dt);
    check(first > still_out[0].m[3][0] + 1e-4f, "y conserva el impulso: tarda mas en volver");
    float x = first;
    for (int f = 0; f < 36; ++f) {
        out = fresh;
        in.apply(out, dt);
        x = out[0].m[3][0];
    }
    check(std::abs(x) < 0.05f * last_x, "a la duracion casi no queda desfase");
    // Giro: de 90 grados a 0 sin salto.
    Inertializer r;
    const Quat q90{0.0f, std::sin(0.785398f), 0.0f, std::cos(0.785398f)};
    std::vector<Mat4> turned(1, core::composeTrs(Vec3{}, q90, Vec3{1.0f}));
    r.apply(turned, dt);
    r.apply(turned, dt);
    r.start(fresh, fresh, 0.2f, dt);
    out = fresh;
    r.apply(out, dt);
    check(out[0].m[0][0] < 0.2f, "el giro empieza desde el que se veia");
    const Vec3 axis = anim::quatToScaledAxis(q90);
    check(std::abs(axis.y - 1.5707963f) < 1e-3f, "eje * angulo de 90 grados");
    // El controlador guarda el modo de mezcla y continuar el ciclo.
    ecs::AnimatorController c;
    c.states.resize(2);
    ecs::AnimatorTransition t;
    t.to = 1;
    t.inertial = false;
    t.sync_phase = true;
    c.transitions.push_back(t);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_inertial_test.cranimator";
    ecs::AnimatorController loaded;
    check(ecs::saveAnimatorController(c, file) && ecs::loadAnimatorController(file, loaded) &&
              !loaded.transitions[0].inertial && loaded.transitions[0].sync_phase,
          "fundido cruzado y continuar el ciclo se guardan");
    std::filesystem::remove(file);
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--dump") return dumpModel(argv[2]);
    testDetection();
    testRetarget();
    testIK();
    testSprings();
    testLegs();
    testChainAndCreature();
    testFootPlacement();
    testPhysBones();
    testRagdollPhysics();
    testGroundTiltAndLook();
    testBlendTrees();
    testInertialization();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
