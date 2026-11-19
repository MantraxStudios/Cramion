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
#include <fstream>
#include <iterator>
#include <unordered_map>
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

namespace {
// --- Animacion de juego: capas, aditivas, root motion, eventos, curvas, montages ---

// Esqueleto de prueba: "Armature" (escala 0.01 de un FBX en cm) -> "Hips" ->
// ("mixamorig:Spine" -> "Arm") y "Leg". Huesos en Hips, Spine, Arm y Leg.
asset::ModelData makeGameplayModel() {
    asset::ModelData m;
    m.nodes.push_back({"Armature", -1, core::composeTrs(Vec3{}, Quat{}, Vec3{0.01f, 0.01f, 0.01f})});
    m.nodes.push_back({"Hips", 0, core::translate(Vec3{0.0f, 100.0f, 0.0f})});
    m.nodes.push_back({"mixamorig:Spine", 1, core::translate(Vec3{0.0f, 20.0f, 0.0f})});
    m.nodes.push_back({"Arm", 2, core::translate(Vec3{20.0f, 30.0f, 0.0f})});
    m.nodes.push_back({"Leg", 1, core::translate(Vec3{10.0f, -10.0f, 0.0f})});
    for (int i = 1; i <= 4; ++i) m.bones.push_back({m.nodes[static_cast<std::size_t>(i)].name, i, Mat4::identity()});

    // 0 "Andar": la cadera avanza 200 cm en Z en 1 s (2 m en el modelo).
    asset::AnimationClip walk;
    walk.name = "Andar";
    walk.duration = 1.0f;
    asset::AnimationChannel hips;
    hips.node = 1;
    hips.positions = {{0.0f, Vec3{0.0f, 100.0f, 0.0f}}, {1.0f, Vec3{0.0f, 100.0f, 200.0f}}};
    hips.rotations = {{0.0f, Quat{}}, {1.0f, Quat{}}};
    walk.channels.push_back(hips);
    walk.events = {{"PieIzq", 0.25f, ""}, {"PieDer", 0.75f, "{\"lado\":1}"}};
    asset::ClipCurve curve;
    curve.name = "Pisada";
    curve.keys = {{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
    walk.curves.push_back(curve);
    m.animations.push_back(walk);

    // 1 "Girar": la cadera gira 90 grados en Y en el sitio.
    asset::AnimationClip turn;
    turn.name = "Girar";
    turn.duration = 1.0f;
    asset::AnimationChannel turn_hips;
    turn_hips.node = 1;
    turn_hips.positions = {{0.0f, Vec3{0.0f, 100.0f, 0.0f}}};
    turn_hips.rotations = {{0.0f, Quat{}}, {0.5f, axisAngle(Vec3{0, 1, 0}, 45.0f)}, {1.0f, axisAngle(Vec3{0, 1, 0}, 90.0f)}};
    turn.channels.push_back(turn_hips);
    m.animations.push_back(turn);

    // 2 "Saludo": el brazo gira 90 grados en Z.
    asset::AnimationClip wave;
    wave.name = "Saludo";
    wave.duration = 1.0f;
    asset::AnimationChannel arm;
    arm.node = 3;
    arm.rotations = {{0.0f, axisAngle(Vec3{0, 0, 1}, 90.0f)}, {1.0f, axisAngle(Vec3{0, 0, 1}, 90.0f)}};
    wave.channels.push_back(arm);
    asset::AnimationChannel leg;  // tambien mueve la pierna (la mascara la quita)
    leg.node = 4;
    leg.rotations = {{0.0f, axisAngle(Vec3{1, 0, 0}, 60.0f)}};
    wave.channels.push_back(leg);
    m.animations.push_back(wave);

    // 3 "Inclinar": la espalda se inclina de 0 a 30 grados en X (aditiva).
    asset::AnimationClip lean;
    lean.name = "Inclinar";
    lean.duration = 1.0f;
    asset::AnimationChannel spine;
    spine.node = 2;
    spine.rotations = {{0.0f, Quat{}}, {1.0f, axisAngle(Vec3{1, 0, 0}, 30.0f)}};
    lean.channels.push_back(spine);
    m.animations.push_back(lean);
    return m;
}

float quatAngleDegrees(const Quat& a, const Quat& b) {
    const float d = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0f * std::acos(std::min(d, 1.0f)) * 180.0f / core::kPi;
}

bool sameMatrices(const std::vector<Mat4>& a, const std::vector<Mat4>& b, float eps) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                if (std::abs(a[i].m[c][r] - b[i].m[c][r]) > eps) return false;
            }
        }
    }
    return true;
}

void testRootMotion() {
    std::printf("\nRoot motion\n");
    const asset::ModelData model = makeGameplayModel();
    const asset::AnimationClip& walk = model.animations[0];
    anim::RootMotion m = anim::clipRootMotion(model, walk, 1, 0.5f, 0.5f, true);
    check(std::abs(m.position.z - 1.0f) < 1e-3f && std::abs(m.position.x) < 1e-4f && std::abs(m.yaw) < 1e-4f,
          "medio segundo de andar: 1 m (con la escala 0.01 del Armature)");
    m = anim::clipRootMotion(model, walk, 1, 0.2f, 0.4f, true);
    check(std::abs(m.position.z - 0.8f) < 1e-3f, "en bucle: de 0.8 a 0.2 pasando por el final = 0.8 m");
    m = anim::clipRootMotion(model, walk, 1, 0.5f, 2.5f, true);
    check(std::abs(m.position.z - 5.0f) < 1e-2f, "dos vueltas y media en un frame: 5 m");
    m = anim::clipRootMotion(model, walk, 1, 0.3f, -0.2f, true);
    check(std::abs(m.position.z + 0.4f) < 1e-3f, "hacia atras: -0.4 m");
    m = anim::clipRootMotion(model, walk, 1, 1.0f, 0.5f, false);
    check(std::abs(m.position.z - 1.0f) < 1e-3f, "sin bucle se recorta al final");
    const asset::AnimationClip& turn = model.animations[1];
    m = anim::clipRootMotion(model, turn, 1, 1.0f, 1.0f, false);
    check(std::abs(m.yaw - core::kPi * 0.5f) < 1e-3f && core::length(m.position) < 1e-4f, "girar: 90 grados en Y");
    const Vec3 r = anim::rotateAboutY(Vec3{0.0f, 0.0f, 1.0f}, core::kPi * 0.5f);
    check(std::abs(r.x - 1.0f) < 1e-5f && std::abs(r.z) < 1e-5f, "rotateAboutY: +Z girado 90 grados = +X (como un cuaternion en Y)");

    // En el animador: la raiz se queda en el sitio y el movimiento sale aparte.
    anim::Animator a(model);
    anim::ClipSample s{0, 0.5f, 1.0f, 0.5f, true};
    a.evaluateBlend(&s, 1);
    check(std::abs(positionOf(a.globals()[1]).z - 0.5f * 2.0f) < 1e-3f, "sin root motion la cadera avanza (como siempre)");
    a.setRootMotionNode(1);
    a.evaluateBlend(&s, 1);
    check(std::abs(positionOf(a.globals()[1]).z) < 1e-4f && std::abs(positionOf(a.globals()[1]).y - 1.0f) < 1e-4f,
          "con root motion la cadera se queda en el sitio (la altura se conserva)");
    check(std::abs(a.rootMotion().position.z - 1.0f) < 1e-3f, "y el avance sale en rootMotion()");
    anim::ClipSample spin{1, 0.5f, 1.0f, 0.5f, false};
    a.evaluateBlend(&spin, 1);
    Vec3 t, sc;
    Quat q;
    // La cadera no gira en la pose (el giro va al personaje).
    const Mat4 hips = a.globals()[1];
    const Vec3 fwd{hips.m[2][0], hips.m[2][1], hips.m[2][2]};
    check(std::abs(fwd.x) < 1e-3f && fwd.z > 0.0f, "con root motion el giro en Y sale de la pose");
    check(std::abs(a.rootMotion().yaw - core::kPi * 0.25f) < 1e-3f, "y va a rootMotion().yaw (45 grados)");
    (void)t;
    (void)sc;
    (void)q;
    // Mezcla: la mitad de andar y la mitad de quieto avanza la mitad.
    anim::ClipSample blend[2] = {{0, 0.5f, 0.5f, 0.5f, true}, {1, 0.0f, 0.5f, 0.0f, false}};
    a.evaluateBlend(blend, 2);
    check(std::abs(a.rootMotion().position.z - 0.5f) < 1e-3f, "mezcla por pesos del movimiento");
    a.setRootMotionNode(-1);
    a.evaluateBlend(&s, 1);
    check(a.rootMotion().position.z == 0.0f, "apagado: sin movimiento");
}

void testLayersAndAdditive() {
    std::printf("\nCapas, mascaras y aditivas\n");
    const asset::ModelData model = makeGameplayModel();
    // Mascaras.
    std::vector<float> w = ecs::buildMaskWeights(model.nodes, {{"Spine", true, 1.0f}});
    check(w[0] == 0.0f && w[1] == 0.0f && w[2] == 1.0f && w[3] == 1.0f && w[4] == 0.0f,
          "mascara Spine con hijos (y el prefijo mixamorig:)");
    w = ecs::buildMaskWeights(model.nodes, {{"Hips", true, 1.0f}, {"Arm", false, 0.0f}});
    check(w[1] == 1.0f && w[2] == 1.0f && w[3] == 0.0f && w[4] == 1.0f, "la ultima entrada manda (quitar el brazo)");
    w = ecs::buildMaskWeights(model.nodes, {{"Spine", false, 0.5f}});
    check(w[2] == 0.5f && w[3] == 0.0f, "sin hijos y con peso");
    w = ecs::buildMaskWeights(model.nodes, {});
    check(w.size() == 5 && w[0] == 1.0f && w[4] == 1.0f, "sin entradas: todo el cuerpo");
    check(ecs::boneNameMatches("Armature|Hips", "Hips") && !ecs::boneNameMatches("MyHips", "Hips"), "nombres con prefijo");

    // Sin capas, evaluateLayered = evaluateBlend.
    anim::Animator a(model);
    anim::Animator b(model);
    anim::LayeredPosePlan plan;
    plan.base = {{0, 0.3f, 0.7f}, {2, 0.4f, 0.3f}};
    a.evaluateLayered(plan);
    b.evaluateBlend(plan.base);
    check(sameMatrices(a.boneMatrices(), b.boneMatrices(), 1e-4f), "sin capas, la misma pose que evaluateBlend");

    // evaluateBlend de un clip = evaluate() del clip (la refactorizacion no cambia nada).
    anim::Animator c(model);
    c.play(0, true);
    c.setTime(0.3f);
    b.evaluateBlend({anim::ClipSample{0, 0.3f, 1.0f}});
    check(sameMatrices(c.boneMatrices(), b.boneMatrices(), 1e-4f), "evaluateBlend de un clip = evaluate()");

    // Override con mascara: el brazo saluda, la pierna sigue la base.
    plan.base = {{0, 0.5f, 1.0f}};
    anim::PoseLayerJob& job = plan.addLayer();
    job.samples = {{2, 0.0f, 1.0f}};
    job.weight = 1.0f;
    job.mask = std::make_shared<const std::vector<float>>(ecs::buildMaskWeights(model.nodes, {{"Spine", true, 1.0f}}));
    a.evaluateLayered(plan);
    anim::LocalPose pose;
    a.samplePose(plan.base.data(), plan.base.size(), pose);
    const std::vector<Mat4> locals = a.locals();
    Vec3 tt, ss;
    (void)tt;
    (void)ss;
    const Mat4 arm_expected = core::composeTrs(Vec3{20.0f, 30.0f, 0.0f}, axisAngle(Vec3{0, 0, 1}, 90.0f), Vec3{1, 1, 1});
    check(sameMatrices({locals[3]}, {arm_expected}, 1e-3f), "override: el brazo de la capa");
    check(sameMatrices({locals[4]}, {model.nodes[4].local}, 1e-4f), "override: la pierna fuera de la mascara sigue la base");
    check(std::abs(positionOf(locals[1]).z - 100.0f) < 1e-2f, "override: la cadera fuera de la mascara sigue andando");
    // A media fuerza: el brazo a 45 grados.
    job.weight = 0.5f;
    a.evaluateLayered(plan);
    const Mat4 arm_half = core::composeTrs(Vec3{20.0f, 30.0f, 0.0f}, axisAngle(Vec3{0, 0, 1}, 45.0f), Vec3{1, 1, 1});
    check(sameMatrices({a.locals()[3]}, {arm_half}, 1e-3f), "override al 50%: medio giro");
    // Peso 0: como si no estuviera.
    job.weight = 0.0f;
    a.evaluateLayered(plan);
    b.evaluateBlend(plan.base);
    check(sameMatrices(a.boneMatrices(), b.boneMatrices(), 1e-4f), "capa a peso 0 = solo la base");

    // Aditiva: inclinar la espalda 30 grados sobre la pose que haya.
    plan.clear();
    plan.base = {{2, 0.0f, 1.0f}};  // el saludo (la espalda sin girar)
    anim::PoseLayerJob& add = plan.addLayer();
    add.samples = {{3, 1.0f, 1.0f}};
    add.mode = anim::LayerBlendMode::Additive;
    add.weight = 1.0f;
    a.evaluateLayered(plan);
    anim::LocalPose result;
    a.samplePose(plan.base.data(), plan.base.size(), result);  // la base sola (para comparar)
    Quat spine_rot{};
    {
        // Rotacion local de la espalda tras la capa: columnas de la matriz.
        const Mat4& l = a.locals()[2];
        const Vec3 y_axis{l.m[1][0], l.m[1][1], l.m[1][2]};
        spine_rot = Quat{};
        check(std::abs(std::acos(std::clamp(y_axis.y / core::length(y_axis), -1.0f, 1.0f)) * 180.0f / core::kPi - 30.0f) < 0.1f,
              "aditiva: la espalda se inclina 30 grados (primer fotograma como referencia)");
    }
    (void)spine_rot;
    check(sameMatrices({a.locals()[3]}, {arm_expected}, 1e-3f), "aditiva: el resto de la pose de la base sigue igual");
    add.weight = 0.5f;
    a.evaluateLayered(plan);
    {
        const Mat4& l = a.locals()[2];
        const Vec3 y_axis{l.m[1][0], l.m[1][1], l.m[1][2]};
        check(std::abs(std::acos(std::clamp(y_axis.y / core::length(y_axis), -1.0f, 1.0f)) * 180.0f / core::kPi - 15.0f) < 0.1f,
              "aditiva al 50%: 15 grados");
    }
    // Referencia de otro clip: Inclinar al final menos Inclinar al final = nada.
    add.weight = 1.0f;
    add.reference = {{3, 1.0f, 1.0f}};
    a.evaluateLayered(plan);
    b.evaluateBlend(plan.base);
    check(sameMatrices(a.boneMatrices(), b.boneMatrices(), 1e-3f), "aditiva con referencia = la misma pose: no cambia nada");
    // Funciones sueltas.
    anim::LocalPose p1, p2;
    a.restPose(p1);
    a.restPose(p2);
    p2.translation[1] = p2.translation[1] + Vec3{0.0f, 0.0f, 10.0f};
    anim::makeAdditivePose(p2, p1);
    check(std::abs(p2.translation[1].z - 10.0f) < 1e-4f && quatAngleDegrees(p2.rotation[1], Quat{}) < 1e-2f,
          "makeAdditivePose resta la referencia");
    anim::applyAdditivePose(p1, p2, 0.5f, nullptr);
    check(std::abs(p1.translation[1].z - 5.0f) < 1e-4f, "applyAdditivePose con peso");

    // Root motion por capas: un montage de cuerpo entero manda en el movimiento.
    a.setRootMotionNode(1);
    plan.clear();
    plan.base = {{1, 0.5f, 1.0f, 0.0f, true}};  // quieto (girar sin avanzar)
    anim::PoseLayerJob& mont = plan.addLayer();
    mont.samples = {{0, 0.5f, 1.0f, 0.5f, false}};
    mont.weight = 0.5f;
    mont.root_motion = true;
    a.evaluateLayered(plan);
    check(std::abs(a.rootMotion().position.z - 0.5f) < 1e-3f, "root motion de un montage al 50%: medio metro");
    mont.mask = std::make_shared<const std::vector<float>>(ecs::buildMaskWeights(model.nodes, {{"Spine", true, 1.0f}}));
    a.evaluateLayered(plan);
    check(std::abs(a.rootMotion().position.z) < 1e-4f, "con una mascara sin la raiz no mueve al personaje");
}

void testClipEvents() {
    std::printf("\nEventos y curvas de los clips\n");
    asset::AnimationClip clip;
    clip.duration = 1.0f;
    clip.events = {{"A", 0.0f, ""}, {"B", 0.25f, ""}, {"C", 0.75f, ""}};
    std::vector<std::uint32_t> out;
    const auto fired = [&](float time, float delta, bool loop, bool start) {
        out.clear();
        anim::collectClipEvents(clip, time, delta, loop, start, out);
        std::string s;
        for (const std::uint32_t k : out) s += clip.events[k].name;
        return s;
    };
    check(fired(0.3f, 0.1f, true, false) == "B", "pasa por B");
    check(fired(0.25f, 0.05f, true, false) == "B", "justo en B (intervalo (a, b])");
    check(fired(0.35f, 0.1f, true, false).empty(), "B no se repite en el frame siguiente");
    check(fired(0.1f, 0.4f, true, false) == "AC", "al dar la vuelta: C y el A del principio");
    check(fired(0.5f, 3.2f, true, false) == "ABC", "varias vueltas en un frame: cada evento UNA vez");
    check(fired(0.0f, 0.0f, true, true) == "A", "al entrar en el estado, los del primer instante");
    check(fired(0.1f, 0.1f, true, false).empty(), "sin include_start el del principio no se repite");
    check(fired(0.2f, -0.1f, true, false) == "B", "hacia atras");
    check(fired(1.0f, 0.5f, false, false) == "C", "sin bucle");
    check(fired(1.0f, 5.0f, false, false) == "BC", "sin bucle no da vueltas (recorta a [0, duracion])");
    check(fired(0.5f, 0.0f, true, false).empty(), "quieto: nada");

    // Curvas.
    asset::ClipCurve curve;
    curve.keys = {{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
    check(std::abs(curve.sample(0.25f) - 0.5f) < 1e-5f && curve.sample(-1.0f) == 0.0f && curve.sample(0.5f) == 1.0f,
          "curva lineal entre claves");

    // De un juego de muestras: el clip que mas pesa da los eventos; curvas por pesos.
    const asset::ModelData model = makeGameplayModel();
    ecs::AnimatorRuntime runtime;
    std::vector<anim::ClipSample> samples = {{0, 0.3f, 0.7f, 0.1f, true}, {1, 0.3f, 0.3f, 0.1f, true}};
    ecs::collectSampleEvents(model, samples.data(), samples.size(), false, 0, runtime);
    check(runtime.events.size() == 1 && runtime.events[0].name == "PieIzq" && runtime.events[0].clip == "Andar",
          "evento del clip que mas pesa, con el nombre del clip");
    check(runtime.events[0].params.empty(), "sin parametros");
    samples[0].weight = 0.2f;
    runtime.events.clear();
    ecs::collectSampleEvents(model, samples.data(), samples.size(), false, 0, runtime);
    check(runtime.events.empty(), "si pesa menos otro clip (sin eventos), no suena");
    std::unordered_map<std::string, float> curves;
    samples = {{0, 0.5f, 0.5f}, {1, 0.5f, 0.5f}};
    ecs::accumulateSampleCurves(model, samples.data(), samples.size(), 1.0f, curves);
    check(curves.count("Pisada") == 1 && std::abs(curves["Pisada"] - 1.0f) < 1e-5f,
          "curva: la media de los clips que la tienen");
    ecs::accumulateSampleCurves(model, samples.data(), 1, 0.5f, curves);
    check(std::abs(curves["Pisada"] - 1.0f) < 1e-5f, "capa encima con el mismo valor");
    runtime.events.clear();
    for (int i = 0; i < 100; ++i) runtime.pushEvent(ecs::AnimatorFiredEvent{"x" + std::to_string(i), "c", "", 0.0f, 0});
    check(runtime.events.size() == ecs::AnimatorRuntime::kMaxPendingEvents && runtime.events.back().name == "x99",
          "la cola de eventos tiene tope (sin Play nadie la vacia)");
}

void testControllerLayers() {
    std::printf("\nCapas del Animator Controller\n");
    const asset::ModelData model = makeGameplayModel();
    ecs::AnimatorController c;
    c.parameters.push_back({"apuntar", ecs::AnimatorParameterType::Bool, 0.0f});
    c.parameters.push_back({"golpe", ecs::AnimatorParameterType::Trigger, 0.0f});
    ecs::AnimatorState idle;
    idle.name = "Andar";
    idle.clip_name = "Andar";
    c.states.push_back(idle);
    ecs::AnimatorLayer torso;
    torso.name = "Torso";
    torso.weight = 0.8f;
    torso.mask_bones = {{"Spine", true, 1.0f}};
    ecs::AnimatorState rest;
    rest.name = "Nada";
    rest.clip_name = "Saludo";
    ecs::AnimatorState aim;
    aim.name = "Apuntar";
    aim.clip_name = "Inclinar";
    torso.states = {rest, aim};
    ecs::AnimatorTransition t;
    t.from = 0;
    t.to = 1;
    t.duration = 0.2f;
    t.conditions.push_back({"apuntar", ecs::AnimatorConditionMode::If, 0.0f});
    torso.transitions.push_back(t);
    c.layers.push_back(torso);
    ecs::AnimatorLayer synced;
    synced.name = "Sincronizada";
    synced.sync_layer = 0;
    synced.blending = ecs::AnimatorLayerBlend::Additive;
    ecs::AnimatorState over;
    over.clip_name = "Girar";
    synced.states = {over};
    c.layers.push_back(synced);
    check(c.findLayer("Base") == 0 && c.findLayer("Torso") == 1 && c.findLayer("Sincronizada") == 2 && c.findLayer("x") == -1,
          "capas por nombre");

    // Guardar y leer.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cramion_anim_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "capas.cranimator";
    check(ecs::saveAnimatorController(c, file), "guardar con capas");
    ecs::AnimatorController loaded;
    check(ecs::loadAnimatorController(file, loaded), "leer con capas");
    check(loaded.layers.size() == 2 && loaded.layers[0].name == "Torso" && std::abs(loaded.layers[0].weight - 0.8f) < 1e-5f &&
              loaded.layers[0].mask_bones.size() == 1 && loaded.layers[0].states.size() == 2 &&
              loaded.layers[0].transitions.size() == 1 && loaded.layers[1].sync_layer == 0 &&
              loaded.layers[1].blending == ecs::AnimatorLayerBlend::Additive,
          "las capas vuelven igual");
    // Sin capas: el archivo de siempre (version 1, sin "layers").
    ecs::AnimatorController plain = c;
    plain.layers.clear();
    const std::filesystem::path plain_file = dir / "sin_capas.cranimator";
    ecs::saveAnimatorController(plain, plain_file);
    std::ifstream in(plain_file);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(text.find("\"layers\"") == std::string::npos && text.find("\"version\": 1") != std::string::npos,
          "sin capas se guarda como antes (version 1)");
    ecs::AnimatorController plain_loaded;
    ecs::loadAnimatorController(plain_file, plain_loaded);
    check(plain_loaded.layers.empty() && plain_loaded.states.size() == 1, "un controlador antiguo = una capa base");

    // Paso de una capa.
    ecs::AnimatorRuntime runtime;
    const ecs::AnimatorClipLookup lookup{
        [&](const std::string& name, const assets::AssetRef&) {
            for (std::size_t i = 0; i < model.animations.size(); ++i) {
                if (model.animations[i].name == name) return static_cast<int>(i);
            }
            return -1;
        },
        [&](int clip) { return model.animations[static_cast<std::size_t>(clip)].duration; }};
    std::vector<anim::ClipSample> out;
    std::vector<float> scratch;
    std::size_t begin = 0;
    ecs::stepAnimatorLayer(c, 1, runtime, 0.1f, 1.0f, lookup, out, begin, nullptr, scratch);
    check(runtime.layers.size() == 2 && runtime.layers[0].state == 0 && runtime.layers[0].entered &&
              out.size() == 1 && out[0].clip == 2,
          "la capa entra en su estado por defecto");
    check(std::abs(ecs::animatorLayerWeight(c, runtime, 1) - 0.8f) < 1e-5f && ecs::animatorLayerWeight(c, runtime, 0) == 1.0f,
          "peso del controlador");
    runtime.layers[0].weight = 0.3f;
    check(std::abs(ecs::animatorLayerWeight(c, runtime, 1) - 0.3f) < 1e-5f, "peso del juego (setLayerWeight)");
    ecs::stepAnimatorLayer(c, 1, runtime, 0.1f, 1.0f, lookup, out, begin, nullptr, scratch);
    check(!runtime.layers[0].entered && std::abs(out[0].delta - 0.1f) < 1e-4f, "avanza (delta del clip)");
    runtime.values["apuntar"] = 1.0f;
    ecs::stepAnimatorLayer(c, 1, runtime, 0.1f, 1.0f, lookup, out, begin, nullptr, scratch);
    float sum = 0.0f;
    for (const anim::ClipSample& s : out) sum += s.weight;
    check(runtime.layers[0].state == 1 && runtime.layers[0].state_name == "Apuntar" && out.size() == 2 && begin == 1 &&
              std::abs(sum - 1.0f) < 1e-4f && out[1].clip == 3,
          "transicion con fundido: el que se va y el que entra (pesos que suman 1)");
    ecs::stepAnimatorLayer(c, 1, runtime, 0.3f, 1.0f, lookup, out, begin, nullptr, scratch);
    check(out.size() == 1 && out[0].clip == 3, "al acabar el fundido, solo el nuevo");
    runtime.layers[0].cross_fade = "Nada";
    runtime.layers[0].cross_fade_time = 0.0f;
    ecs::stepAnimatorLayer(c, 1, runtime, 0.1f, 1.0f, lookup, out, begin, nullptr, scratch);
    check(runtime.layers[0].state == 0 && out.size() == 1 && out[0].clip == 2, "crossFadeLayer en seco");
    // Sincronizada con la base.
    ecs::AnimatorSyncSource sync;
    sync.state = 0;
    sync.phase = 0.4f;
    sync.dphase = 0.1f;
    ecs::stepAnimatorLayer(c, 2, runtime, 0.1f, 1.0f, lookup, out, begin, &sync, scratch);
    check(out.size() == 1 && out[0].clip == 1 && std::abs(out[0].time - 0.4f) < 1e-5f && runtime.layers[1].state == 0,
          "sincronizada: el estado y la fase de la base con su propio clip");
    // removeAnimatorState en una capa.
    ecs::AnimatorLayer copy = torso;
    ecs::removeAnimatorState(copy.states, copy.transitions, copy.default_state, 0);
    check(copy.states.size() == 1 && copy.transitions.empty() && copy.default_state == 0, "quitar un estado de una capa");

    // Revision: un estado sin bucle parado al final no sigue avanzando (antes
    // el delta seguia siendo dt: el evento del ultimo fotograma sonaba en cada
    // frame y el root motion seguia moviendo al personaje).
    {
        ecs::AnimatorController once;
        once.states.push_back(idle);
        ecs::AnimatorLayer layer;
        layer.name = "Una vez";
        ecs::AnimatorState walk_once;
        walk_once.name = "Andar una vez";
        walk_once.clip_name = "Andar";
        walk_once.loop = false;
        layer.states = {walk_once};
        once.layers.push_back(layer);
        ecs::AnimatorRuntime r;
        std::vector<anim::ClipSample> o;
        std::size_t b = 0;
        ecs::stepAnimatorLayer(once, 1, r, 0.0f, 1.0f, lookup, o, b, nullptr, scratch);  // entra
        ecs::stepAnimatorLayer(once, 1, r, 0.9f, 1.0f, lookup, o, b, nullptr, scratch);
        ecs::stepAnimatorLayer(once, 1, r, 0.3f, 1.0f, lookup, o, b, nullptr, scratch);
        check(o.size() == 1 && std::abs(o[0].time - 1.0f) < 1e-4f && std::abs(o[0].delta - 0.1f) < 1e-4f && !o[0].loop,
              "sin bucle: al llegar al final el delta es solo lo que quedaba");
        ecs::AnimatorRuntime events_rt;
        ecs::collectSampleEvents(model, o.data(), o.size(), false, 1, events_rt);
        ecs::stepAnimatorLayer(once, 1, r, 0.1f, 1.0f, lookup, o, b, nullptr, scratch);
        check(o.size() == 1 && o[0].delta == 0.0f, "sin bucle y parado al final: delta 0");
        events_rt.events.clear();
        ecs::collectSampleEvents(model, o.data(), o.size(), false, 1, events_rt);
        check(events_rt.events.empty(), "parado al final: los eventos no se repiten cada frame");
        check(anim::clipRootMotion(model, model.animations[0], 1, o[0].time, o[0].delta, false).position.z == 0.0f,
              "parado al final: sin root motion");
    }

    // Mascara reutilizable (.crmask).
    ecs::BoneMask mask;
    mask.bones = {{"Spine", true, 1.0f}, {"Arm", false, 0.0f}};
    const std::filesystem::path mask_file = dir / "torso.crmask";
    check(ecs::saveBoneMask(mask, mask_file), "guardar .crmask");
    ecs::BoneMask mask_loaded;
    check(ecs::loadBoneMask(mask_file, mask_loaded) && mask_loaded.uuid.valid() && mask_loaded.bones.size() == 2 &&
              !mask_loaded.bones[1].children && mask_loaded.bones[1].weight == 0.0f,
          "leer .crmask (con UUID)");
}

void testMontages() {
    std::printf("\nMontages\n");
    asset::AnimationClip clip;
    clip.name = "Golpe";
    clip.duration = 1.0f;
    clip.events = {{"Impacto", 0.4f, ""}, {"Fin", 0.9f, ""}};
    ecs::AnimatorMontage m;
    m.blend_in = 0.1f;
    m.blend_out = 0.2f;
    ecs::startMontage(m);
    check(m.active && m.just_started && m.serial == 1, "playMontage lo deja listo");
    std::vector<std::uint32_t> ev;
    ecs::stepMontage(m, clip, 0.016f, &ev);
    check(m.time == 0.0f && m.weight == 0.0f && ev.empty(), "primer paso: empieza en 0 con peso 0");
    ecs::stepMontage(m, clip, 0.05f, &ev);
    check(m.weight > 0.0f && m.weight < 1.0f, "entrando (fundido de entrada)");
    ecs::stepMontage(m, clip, 0.1f, &ev);
    check(std::abs(m.weight - 1.0f) < 1e-5f, "dentro: peso 1");
    ev.clear();
    ecs::stepMontage(m, clip, 0.3f, &ev);
    check(ev.size() == 1 && clip.events[ev[0]].name == "Impacto" && std::abs(m.delta - 0.3f) < 1e-5f,
          "el evento del golpe suena una vez");
    ev.clear();
    ecs::stepMontage(m, clip, 0.4f, &ev);  // t = 0.85: saliendo
    check(m.weight < 1.0f && m.weight > 0.0f && m.active, "fundido de salida al final del clip");
    ecs::stepMontage(m, clip, 0.5f, &ev);
    check(!m.active && m.weight == 0.0f && ev.size() == 1, "acaba y vuelve al grafo (y suena Fin)");

    // Secciones: Inicio -> Bucle (se repite) -> Final.
    clip.sections = {{"Inicio", 0.0f, ""}, {"Bucle", 0.3f, "Bucle"}, {"Final", 0.6f, ""}};
    ecs::startMontage(m);
    m.blend_in = 0.0f;
    ecs::stepMontage(m, clip, 0.016f, nullptr);
    ecs::stepMontage(m, clip, 0.5f, nullptr);  // 0.5: en Bucle
    check(m.section == "Bucle" && std::abs(m.time - 0.5f) < 1e-5f, "en la seccion Bucle");
    ecs::stepMontage(m, clip, 0.2f, nullptr);  // pasa de 0.6: vuelve a 0.3 + 0.1
    check(m.section == "Bucle" && std::abs(m.time - 0.4f) < 1e-4f && m.active && m.weight == 1.0f,
          "la seccion en bucle vuelve a empezar (sin fundido de salida)");
    m.jump_section = "Final";
    ecs::stepMontage(m, clip, 0.016f, nullptr);
    check(m.section == "Final" && std::abs(m.time - 0.6f) < 1e-5f, "montageJumpToSection");
    ecs::stepMontage(m, clip, 0.5f, nullptr);
    check(!m.active, "la ultima seccion acaba el montage");
    // Empezar en una seccion y pararlo.
    m.start_section = "Final";
    ecs::startMontage(m);
    ecs::stepMontage(m, clip, 0.016f, nullptr);
    check(std::abs(m.time - 0.6f) < 1e-5f, "empezar en una seccion");
    m.stopping = true;
    m.stop_blend = 0.1f;
    m.stop_elapsed = 0.0f;
    ecs::stepMontage(m, clip, 0.05f, nullptr);
    check(m.active && m.weight < 1.0f, "stopMontage se apaga poco a poco");
    ecs::stepMontage(m, clip, 0.06f, nullptr);
    check(!m.active, "y se acaba");
    check(ecs::clipSectionAt(clip, 0.45f) == 1 && ecs::clipSectionAt(clip, 0.0f) == 0, "seccion de un instante");
}

void testClipMetaFiles() {
    std::printf("\nEventos y curvas en archivos\n");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cramion_anim_tests";
    std::filesystem::create_directories(dir);
    asset::ModelData model = makeGameplayModel();
    // .cranim con meta en la cabecera.
    const std::filesystem::path file = dir / "andar.cranim";
    check(ecs::saveAnimationClip(model, model.animations[0], file), "guardar .cranim con eventos");
    asset::AnimationClip loaded;
    check(ecs::loadAnimationClip(file, model, loaded), "leer .cranim");
    check(loaded.events.size() == 2 && loaded.events[1].name == "PieDer" && loaded.events[1].params == "{\"lado\":1}" &&
              loaded.curves.size() == 1 && loaded.curves[0].keys.size() == 3 && loaded.channels.size() == 1,
          "los eventos y curvas vuelven con el clip");
    // La cabecera sigue siendo una linea (la base de datos lee solo esa).
    {
        std::ifstream in(file);
        std::string first, second;
        std::getline(in, first);
        std::getline(in, second);
        check(first.find("\"meta\"") != std::string::npos && first.find("\"uuid\"") != std::string::npos &&
                  second.find("\"channels\"") != std::string::npos,
              "meta en la primera linea, pistas en la segunda");
    }
    // Un .cranim de antes (sin meta) se lee igual.
    asset::AnimationClip plain = model.animations[0];
    plain.events.clear();
    plain.curves.clear();
    const std::filesystem::path old_file = dir / "viejo.cranim";
    ecs::saveAnimationClip(model, plain, old_file);
    {
        std::ifstream in(old_file);
        std::string first;
        std::getline(in, first);
        check(first.find("\"meta\"") == std::string::npos, "sin eventos no se escribe meta (archivo como antes)");
    }
    // Editar solo la meta.
    ecs::ClipMeta meta;
    check(ecs::loadClipMetaFromCranim(old_file, meta) && meta.empty(), "leer meta vacia");
    const int i0 = ecs::addClipEvent(meta, "Golpe", 0.6f, 1.0f);
    const int i1 = ecs::addClipEvent(meta, "Paso", 0.2f, 1.0f);
    check(i0 == 0 && i1 == 0 && meta.events[1].name == "Golpe", "anadir eventos ordenados por tiempo");
    const int moved = ecs::moveClipEvent(meta, 0, 0.9f, 1.0f);
    check(moved == 1 && meta.events[1].name == "Paso" && std::abs(meta.events[1].time - 0.9f) < 1e-6f, "mover un evento");
    check(ecs::moveClipEvent(meta, 0, 5.0f, 1.0f) == 1 && meta.events[1].time == 1.0f, "mover recorta a la duracion");
    meta.sections.push_back({"Inicio", 0.0f, ""});
    check(ecs::saveClipMetaToCranim(old_file, meta), "guardar la meta en el .cranim");
    ecs::ClipMeta again;
    check(ecs::loadClipMetaFromCranim(old_file, again) && again.events.size() == 2 && again.sections.size() == 1,
          "la meta vuelve");
    asset::AnimationClip relo;
    check(ecs::loadAnimationClip(old_file, model, relo) && relo.channels.size() == 1 && relo.events.size() == 2,
          "las pistas siguen intactas tras guardar la meta");
    check(ecs::removeClipEvent(meta, 0) && meta.events.size() == 1 && !ecs::removeClipEvent(meta, 7), "quitar un evento");
    // Archivo lateral del modelo.
    const std::filesystem::path sidecar = ecs::clipMetaSidecarPath(dir / "Heroe.crdata");
    check(sidecar.filename().string() == "Heroe.crdata.crclipmeta", "nombre del archivo lateral");
    std::map<std::string, ecs::ClipMeta> clips;
    clips["Andar"] = again;
    clips["Vacio"] = ecs::ClipMeta{};
    check(ecs::saveModelClipMeta(sidecar, clips), "guardar el archivo lateral");
    std::map<std::string, ecs::ClipMeta> clips_loaded;
    check(ecs::loadModelClipMeta(sidecar, clips_loaded) && clips_loaded.size() == 2 &&
              clips_loaded["Andar"].events.size() == 2 && clips_loaded["Vacio"].empty(),
          "leer el archivo lateral (los vacios tambien: al recargar se borran)");
    asset::AnimationClip target = model.animations[1];
    ecs::applyClipMeta(target, clips_loaded["Andar"]);
    check(target.events.size() == 2 && target.events[0].time <= target.events[1].time, "aplicar la meta a un clip");

    // Linea de tiempo y reproduccion de la ventana de clips.
    check(std::abs(ecs::timelineTimeAt(150.0f, 100.0f, 200.0f, 2.0f, 0.0f) - 0.5f) < 1e-5f, "linea de tiempo: x -> tiempo");
    check(std::abs(ecs::timelineTimeAt(151.0f, 100.0f, 200.0f, 2.0f, 30.0f) - 15.0f / 30.0f) < 1e-5f, "al fotograma");
    check(ecs::timelineTimeAt(0.0f, 100.0f, 200.0f, 2.0f, 0.0f) == 0.0f && ecs::timelineTimeAt(999.0f, 100.0f, 200.0f, 2.0f, 0.0f) == 2.0f,
          "recortada");
    check(std::abs(ecs::timelineX(1.0f, 100.0f, 200.0f, 2.0f) - 200.0f) < 1e-4f, "tiempo -> x");
    ecs::ClipPreviewPlayer player;
    player.playing = true;
    player.step(0.75f, 1.0f);
    player.step(0.5f, 1.0f);
    check(std::abs(player.time - 0.25f) < 1e-5f && player.playing, "vista previa en bucle");
    player.loop = false;
    player.step(1.0f, 1.0f);
    check(player.time == 1.0f && !player.playing, "sin bucle se para al final");
}

}  // namespace

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
    testRootMotion();
    testLayersAndAdditive();
    testClipEvents();
    testControllerLayers();
    testMontages();
    testClipMetaFiles();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
