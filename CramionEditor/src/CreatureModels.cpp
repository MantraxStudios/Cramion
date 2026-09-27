#include "CreatureModels.h"

#include <CramionCore/asset/AssetManager.h>
#include <CramionCore/asset/Importer.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace cramion::editor {

using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

struct Joint {
    const char* name;
    const char* parent;
    Vec3 position;  // en reposo, espacio del modelo
};

// Pieza de la malla: una caja de `from` a `to` (nombres de articulaciones),
// pegada al hueso `from`.
struct Piece {
    const char* from;
    const char* to;
    float width;   // medio ancho
    float height;  // medio alto
    int material;
};

Quat axisAngle(const Vec3& axis, float degrees) {
    const float half = degrees * core::kPi / 360.0f;
    const Vec3 a = core::normalize(axis) * std::sin(half);
    return Quat{a.x, a.y, a.z, std::cos(half)};
}

class Builder {
public:
    asset::ModelData model;
    std::map<std::string, int> node;  // nombre -> nodo
    std::vector<Vec3> rest;           // posicion de reposo de cada nodo

    void skeleton(const std::vector<Joint>& joints) {
        model.nodes.push_back(asset::Node{"Armature", -1, Mat4::identity()});
        rest.push_back(Vec3{});
        for (const Joint& j : joints) {
            const int parent = std::string(j.parent).empty() ? 0 : node.at(j.parent);
            const Vec3 local = j.position - rest[static_cast<std::size_t>(parent)];
            model.nodes.push_back(asset::Node{j.name, parent, core::translate(local)});
            rest.push_back(j.position);
            node[j.name] = static_cast<int>(model.nodes.size() - 1);
        }
        // Todos los nodos del esqueleto son huesos (se ven y se pueden mover).
        for (std::size_t i = 1; i < model.nodes.size(); ++i) {
            asset::Bone bone;
            bone.name = model.nodes[i].name;
            bone.node = static_cast<int>(i);
            bone.offset = core::translate(rest[i] * -1.0f);  // inversa de la global de reposo
            bone_of_node_[static_cast<int>(i)] = static_cast<std::uint32_t>(i - 1);
            model.bones.push_back(bone);
        }
    }

    void material(const std::string& name, const Vec3& srgb, float roughness) {
        asset::MaterialData m;
        m.name = name;
        m.base_color = core::Vec4{std::pow(srgb.x, 2.2f), std::pow(srgb.y, 2.2f), std::pow(srgb.z, 2.2f), 1.0f};
        m.roughness = roughness;
        model.materials.push_back(m);
        indices_.emplace_back();
    }

    // Caja de a a b (medio ancho w, medio alto h), un poco mas larga por los
    // extremos para que las articulaciones no dejen huecos.
    void box(const Vec3& a0, const Vec3& b0, float w, float h, std::uint32_t bone, int mat) {
        Vec3 d = b0 - a0;
        const float len = core::length(d);
        if (len < 1e-5f) return;
        d = d * (1.0f / len);
        const Vec3 a = a0 - d * std::min(w, h) * 0.6f;
        const Vec3 b = b0 + d * std::min(w, h) * 0.6f;
        const Vec3 ref = std::abs(d.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
        const Vec3 side = core::normalize(core::cross(d, ref)) * w;
        const Vec3 up = core::normalize(core::cross(side, d)) * h;
        const Vec3 c[8] = {a - side - up, a + side - up, a + side + up, a - side + up,
                           b - side - up, b + side - up, b + side + up, b - side + up};
        const int faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
        const Vec3 center = (a + b) * 0.5f;
        for (const auto& f : faces) {
            Vec3 v[4] = {c[f[0]], c[f[1]], c[f[2]], c[f[3]]};
            Vec3 normal = core::normalize(core::cross(v[1] - v[0], v[2] - v[0]));
            const Vec3 face_center = (v[0] + v[1] + v[2] + v[3]) * 0.25f;
            if (core::dot(normal, face_center - center) < 0.0f) {
                std::swap(v[1], v[3]);
                normal = normal * -1.0f;
            }
            const std::uint32_t base = static_cast<std::uint32_t>(model.vertices.size());
            const core::Vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int k = 0; k < 4; ++k) {
                asset::SkinnedVertex vertex{};
                vertex.position = v[k];
                vertex.normal = normal;
                vertex.uv = uvs[k];
                const Vec3 t = core::normalize(v[1] - v[0]);
                vertex.tangent = core::Vec4{t.x, t.y, t.z, 1.0f};
                vertex.joints[0] = bone;
                vertex.weights[0] = 1.0f;
                model.vertices.push_back(vertex);
            }
            std::vector<std::uint32_t>& list = indices_[static_cast<std::size_t>(mat)];
            for (const std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) list.push_back(base + i);
        }
    }

    void pieces(const std::vector<Piece>& list) {
        for (const Piece& p : list) {
            const int from = node.at(p.from);
            box(rest[static_cast<std::size_t>(from)], rest[static_cast<std::size_t>(node.at(p.to))], p.width, p.height,
                bone_of_node_.at(from), p.material);
        }
    }

    // Caja pegada a un hueso en un punto (ojos, nariz).
    void blob(const char* bone, const Vec3& center, float size, int mat) {
        box(center - Vec3{0.0f, 0.0f, size}, center + Vec3{0.0f, 0.0f, size}, size, size, bone_of_node_.at(node.at(bone)), mat);
    }

    // Clip: cada frame (0..1 del ciclo) pone giros (grados sobre un eje del
    // modelo) y desplazamientos a los nodos.
    struct Pose {
        std::map<std::string, Quat> rotation;
        std::map<std::string, Vec3> offset;
    };
    template <typename Fn>
    void clip(const std::string& name, float duration, int samples, Fn&& pose_at) {
        asset::AnimationClip clip;
        clip.name = name;
        clip.duration = duration;
        std::map<int, asset::AnimationChannel> channels;
        for (int s = 0; s <= samples; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(samples);
            Pose pose;
            pose_at(t, pose);
            for (const auto& [n, q] : pose.rotation) {
                asset::AnimationChannel& ch = channels[node.at(n)];
                ch.node = node.at(n);
                ch.rotations.push_back(asset::QuatKey{t * duration, q});
            }
            for (const auto& [n, o] : pose.offset) {
                const int id = node.at(n);
                asset::AnimationChannel& ch = channels[id];
                ch.node = id;
                const Mat4& local = model.nodes[static_cast<std::size_t>(id)].local;
                ch.positions.push_back(asset::VectorKey{t * duration, Vec3{local.m[3][0], local.m[3][1], local.m[3][2]} + o});
            }
        }
        for (auto& [id, ch] : channels) clip.channels.push_back(std::move(ch));
        model.animations.push_back(std::move(clip));
    }

    void finish(const std::string& name) {
        model.name = name;
        for (std::size_t m = 0; m < indices_.size(); ++m) {
            if (indices_[m].empty()) continue;
            asset::SubMesh sub;
            sub.first_index = static_cast<std::uint32_t>(model.indices.size());
            sub.index_count = static_cast<std::uint32_t>(indices_[m].size());
            sub.material = static_cast<std::uint32_t>(m);
            Vec3 lo{1e30f, 1e30f, 1e30f};
            Vec3 hi{-1e30f, -1e30f, -1e30f};
            for (const std::uint32_t i : indices_[m]) {
                const Vec3& p = model.vertices[i].position;
                lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            sub.bounds_min = lo;
            sub.bounds_max = hi;
            model.indices.insert(model.indices.end(), indices_[m].begin(), indices_[m].end());
            model.submeshes.push_back(sub);
        }
    }

private:
    std::vector<std::vector<std::uint32_t>> indices_;
    std::map<int, std::uint32_t> bone_of_node_;
};

const Vec3 kSide{1.0f, 0.0f, 0.0f};   // eje de los giros de las patas (adelante/atras)
const Vec3 kUp{0.0f, 1.0f, 0.0f};

}  // namespace

asset::ModelData makeDogModel() {
    Builder b;
    // Mira a -Z; la izquierda del perro esta en -X.
    b.skeleton({
        {"Body", "", {0.0f, 0.5f, 0.25f}},
        {"Spine1", "Body", {0.0f, 0.525f, 0.05f}},
        {"Chest", "Spine1", {0.0f, 0.55f, -0.2f}},
        {"Neck1", "Chest", {0.0f, 0.66f, -0.3f}},
        {"Neck2", "Neck1", {0.0f, 0.78f, -0.37f}},
        {"Head", "Neck2", {0.0f, 0.86f, -0.46f}},
        {"Snout", "Head", {0.0f, 0.82f, -0.62f}},
        {"Ear_L1", "Head", {-0.06f, 0.93f, -0.44f}},
        {"Ear_L2", "Ear_L1", {-0.09f, 1.02f, -0.43f}},
        {"Ear_L3", "Ear_L2", {-0.11f, 1.08f, -0.42f}},
        {"Ear_R1", "Head", {0.06f, 0.93f, -0.44f}},
        {"Ear_R2", "Ear_R1", {0.09f, 1.02f, -0.43f}},
        {"Ear_R3", "Ear_R2", {0.11f, 1.08f, -0.42f}},
        {"FrontLeg_L1", "Chest", {-0.1f, 0.5f, -0.22f}},
        {"FrontLeg_L2", "FrontLeg_L1", {-0.1f, 0.3f, -0.25f}},
        {"FrontLeg_L3", "FrontLeg_L2", {-0.1f, 0.15f, -0.22f}},
        {"FrontFoot_L", "FrontLeg_L3", {-0.1f, 0.04f, -0.25f}},
        {"FrontToe_L", "FrontFoot_L", {-0.1f, 0.0f, -0.31f}},
        {"FrontLeg_R1", "Chest", {0.1f, 0.5f, -0.22f}},
        {"FrontLeg_R2", "FrontLeg_R1", {0.1f, 0.3f, -0.25f}},
        {"FrontLeg_R3", "FrontLeg_R2", {0.1f, 0.15f, -0.22f}},
        {"FrontFoot_R", "FrontLeg_R3", {0.1f, 0.04f, -0.25f}},
        {"FrontToe_R", "FrontFoot_R", {0.1f, 0.0f, -0.31f}},
        {"HindLeg_L1", "Body", {-0.1f, 0.47f, 0.28f}},
        {"HindLeg_L2", "HindLeg_L1", {-0.1f, 0.3f, 0.2f}},
        {"HindLeg_L3", "HindLeg_L2", {-0.1f, 0.15f, 0.3f}},
        {"HindFoot_L", "HindLeg_L3", {-0.1f, 0.04f, 0.28f}},
        {"HindToe_L", "HindFoot_L", {-0.1f, 0.0f, 0.22f}},
        {"HindLeg_R1", "Body", {0.1f, 0.47f, 0.28f}},
        {"HindLeg_R2", "HindLeg_R1", {0.1f, 0.3f, 0.2f}},
        {"HindLeg_R3", "HindLeg_R2", {0.1f, 0.15f, 0.3f}},
        {"HindFoot_R", "HindLeg_R3", {0.1f, 0.04f, 0.28f}},
        {"HindToe_R", "HindFoot_R", {0.1f, 0.0f, 0.22f}},
        {"Tail1", "Body", {0.0f, 0.53f, 0.4f}},
        {"Tail2", "Tail1", {0.0f, 0.56f, 0.52f}},
        {"Tail3", "Tail2", {0.0f, 0.57f, 0.64f}},
        {"Tail4", "Tail3", {0.0f, 0.56f, 0.75f}},
        {"TailEnd", "Tail4", {0.0f, 0.54f, 0.85f}},
    });
    b.material("Pelo", Vec3{0.58f, 0.37f, 0.19f}, 0.85f);
    b.material("Oscuro", Vec3{0.12f, 0.08f, 0.06f}, 0.6f);
    b.material("Crema", Vec3{0.9f, 0.8f, 0.62f}, 0.8f);
    b.pieces({
        {"Body", "Spine1", 0.12f, 0.13f, 0},
        {"Spine1", "Chest", 0.12f, 0.13f, 0},
        {"Chest", "Neck1", 0.1f, 0.11f, 0},
        {"Neck1", "Neck2", 0.06f, 0.06f, 0},
        {"Neck2", "Head", 0.06f, 0.06f, 0},
        {"Head", "Snout", 0.075f, 0.07f, 0},
        {"Ear_L1", "Ear_L2", 0.035f, 0.012f, 1},
        {"Ear_L2", "Ear_L3", 0.028f, 0.01f, 1},
        {"Ear_R1", "Ear_R2", 0.035f, 0.012f, 1},
        {"Ear_R2", "Ear_R3", 0.028f, 0.01f, 1},
        {"FrontLeg_L1", "FrontLeg_L2", 0.042f, 0.042f, 0},
        {"FrontLeg_L2", "FrontLeg_L3", 0.035f, 0.035f, 0},
        {"FrontLeg_L3", "FrontFoot_L", 0.03f, 0.03f, 0},
        {"FrontFoot_L", "FrontToe_L", 0.035f, 0.022f, 2},
        {"FrontLeg_R1", "FrontLeg_R2", 0.042f, 0.042f, 0},
        {"FrontLeg_R2", "FrontLeg_R3", 0.035f, 0.035f, 0},
        {"FrontLeg_R3", "FrontFoot_R", 0.03f, 0.03f, 0},
        {"FrontFoot_R", "FrontToe_R", 0.035f, 0.022f, 2},
        {"HindLeg_L1", "HindLeg_L2", 0.05f, 0.05f, 0},
        {"HindLeg_L2", "HindLeg_L3", 0.035f, 0.035f, 0},
        {"HindLeg_L3", "HindFoot_L", 0.03f, 0.03f, 0},
        {"HindFoot_L", "HindToe_L", 0.035f, 0.022f, 2},
        {"HindLeg_R1", "HindLeg_R2", 0.05f, 0.05f, 0},
        {"HindLeg_R2", "HindLeg_R3", 0.035f, 0.035f, 0},
        {"HindLeg_R3", "HindFoot_R", 0.03f, 0.03f, 0},
        {"HindFoot_R", "HindToe_R", 0.035f, 0.022f, 2},
        {"Tail1", "Tail2", 0.03f, 0.03f, 0},
        {"Tail2", "Tail3", 0.026f, 0.026f, 0},
        {"Tail3", "Tail4", 0.022f, 0.022f, 0},
        {"Tail4", "TailEnd", 0.018f, 0.018f, 2},
    });
    b.blob("Head", Vec3{-0.045f, 0.9f, -0.53f}, 0.014f, 1);  // ojos
    b.blob("Head", Vec3{0.045f, 0.9f, -0.53f}, 0.014f, 1);
    b.blob("Snout", Vec3{0.0f, 0.84f, -0.67f}, 0.02f, 1);   // nariz
    b.blob("Chest", Vec3{0.0f, 0.5f, -0.3f}, 0.05f, 2);      // pecho claro

    // Caminar: patas en diagonal, el cuerpo sube y baja, la cola se mueve.
    b.clip("Caminar", 0.8f, 32, [](float t, Builder::Pose& p) {
        const float a = t * 2.0f * core::kPi;
        const auto leg = [&](const std::string& side, float phase) {
            const float s = std::sin(a + phase);
            p.rotation[side + "1"] = axisAngle(kSide, 24.0f * s);
            p.rotation[side + "2"] = axisAngle(kSide, -14.0f * (0.5f + 0.5f * std::cos(a + phase)));
        };
        leg("FrontLeg_L", 0.0f);
        leg("HindLeg_R", 0.0f);
        leg("FrontLeg_R", core::kPi);
        leg("HindLeg_L", core::kPi);
        p.offset["Body"] = Vec3{0.0f, 0.012f * std::cos(2.0f * a), 0.0f};
        p.rotation["Tail1"] = axisAngle(kUp, 15.0f * std::sin(a));
        p.rotation["Neck1"] = axisAngle(kSide, 4.0f * std::sin(2.0f * a));
    });
    // Quieto: respira y mueve la cola contento.
    b.clip("Quieto", 2.0f, 40, [](float t, Builder::Pose& p) {
        const float a = t * 2.0f * core::kPi;
        p.rotation["Tail1"] = axisAngle(kUp, 28.0f * std::sin(4.0f * a));
        p.rotation["Chest"] = axisAngle(kSide, 1.5f * std::sin(a));
        p.rotation["Neck2"] = axisAngle(kUp, 6.0f * std::sin(a));
    });
    b.finish("Perro");
    return std::move(b.model);
}

asset::ModelData makeDummyModel() {
    Builder b;
    // Humanoide de 1.75 m con nombres de Mixamo; mira a -Z, su izquierda en -X.
    b.skeleton({
        {"Hips", "", {0.0f, 0.95f, 0.0f}},
        {"Spine", "Hips", {0.0f, 1.07f, 0.0f}},
        {"Spine1", "Spine", {0.0f, 1.25f, 0.0f}},
        {"Neck", "Spine1", {0.0f, 1.45f, 0.0f}},
        {"Head", "Neck", {0.0f, 1.55f, 0.0f}},
        {"HeadTop_End", "Head", {0.0f, 1.76f, 0.0f}},
        {"LeftShoulder", "Spine1", {-0.06f, 1.4f, 0.0f}},
        {"LeftArm", "LeftShoulder", {-0.18f, 1.4f, 0.0f}},
        {"LeftForeArm", "LeftArm", {-0.24f, 1.13f, 0.0f}},
        {"LeftHand", "LeftForeArm", {-0.27f, 0.88f, -0.02f}},
        {"LeftHandEnd", "LeftHand", {-0.28f, 0.78f, -0.02f}},
        {"RightShoulder", "Spine1", {0.06f, 1.4f, 0.0f}},
        {"RightArm", "RightShoulder", {0.18f, 1.4f, 0.0f}},
        {"RightForeArm", "RightArm", {0.24f, 1.13f, 0.0f}},
        {"RightHand", "RightForeArm", {0.27f, 0.88f, -0.02f}},
        {"RightHandEnd", "RightHand", {0.28f, 0.78f, -0.02f}},
        {"LeftUpLeg", "Hips", {-0.1f, 0.92f, 0.0f}},
        {"LeftLeg", "LeftUpLeg", {-0.1f, 0.5f, -0.02f}},
        {"LeftFoot", "LeftLeg", {-0.1f, 0.08f, 0.0f}},
        {"LeftToeBase", "LeftFoot", {-0.1f, 0.0f, -0.12f}},
        {"LeftToe_End", "LeftToeBase", {-0.1f, 0.0f, -0.2f}},
        {"RightUpLeg", "Hips", {0.1f, 0.92f, 0.0f}},
        {"RightLeg", "RightUpLeg", {0.1f, 0.5f, -0.02f}},
        {"RightFoot", "RightLeg", {0.1f, 0.08f, 0.0f}},
        {"RightToeBase", "RightFoot", {0.1f, 0.0f, -0.12f}},
        {"RightToe_End", "RightToeBase", {0.1f, 0.0f, -0.2f}},
        {"Hair1", "Head", {0.0f, 1.66f, 0.1f}},
        {"Hair2", "Hair1", {0.0f, 1.54f, 0.16f}},
        {"Hair3", "Hair2", {0.0f, 1.42f, 0.18f}},
        {"HairEnd", "Hair3", {0.0f, 1.31f, 0.19f}},
    });
    b.material("Maniqui", Vec3{0.78f, 0.74f, 0.68f}, 0.55f);
    b.material("Pelo", Vec3{0.22f, 0.15f, 0.1f}, 0.7f);
    b.material("Articulaciones", Vec3{0.3f, 0.34f, 0.4f}, 0.4f);
    b.pieces({
        {"Hips", "Spine", 0.14f, 0.1f, 0},
        {"Spine", "Spine1", 0.14f, 0.1f, 0},
        {"Spine1", "Neck", 0.17f, 0.1f, 0},
        {"Neck", "Head", 0.045f, 0.045f, 2},
        {"Head", "HeadTop_End", 0.09f, 0.1f, 0},
        {"LeftShoulder", "LeftArm", 0.04f, 0.04f, 2},
        {"LeftArm", "LeftForeArm", 0.045f, 0.045f, 0},
        {"LeftForeArm", "LeftHand", 0.04f, 0.04f, 0},
        {"LeftHand", "LeftHandEnd", 0.04f, 0.015f, 2},
        {"RightShoulder", "RightArm", 0.04f, 0.04f, 2},
        {"RightArm", "RightForeArm", 0.045f, 0.045f, 0},
        {"RightForeArm", "RightHand", 0.04f, 0.04f, 0},
        {"RightHand", "RightHandEnd", 0.04f, 0.015f, 2},
        {"LeftUpLeg", "LeftLeg", 0.07f, 0.07f, 0},
        {"LeftLeg", "LeftFoot", 0.055f, 0.055f, 0},
        {"LeftFoot", "LeftToeBase", 0.045f, 0.035f, 2},
        {"LeftToeBase", "LeftToe_End", 0.04f, 0.02f, 2},
        {"RightUpLeg", "RightLeg", 0.07f, 0.07f, 0},
        {"RightLeg", "RightFoot", 0.055f, 0.055f, 0},
        {"RightFoot", "RightToeBase", 0.045f, 0.035f, 2},
        {"RightToeBase", "RightToe_End", 0.04f, 0.02f, 2},
        {"Hair1", "Hair2", 0.035f, 0.03f, 1},
        {"Hair2", "Hair3", 0.03f, 0.028f, 1},
        {"Hair3", "HairEnd", 0.025f, 0.022f, 1},
    });
    b.blob("Head", Vec3{0.0f, 1.66f, 0.02f}, 0.1f, 1);  // pelo sobre la cabeza
    b.clip("Reposo", 3.0f, 45, [](float t, Builder::Pose& p) {
        const float a = t * 2.0f * core::kPi;
        p.rotation["Spine1"] = axisAngle(kSide, 2.0f * std::sin(a));
        p.rotation["Head"] = axisAngle(kUp, 10.0f * std::sin(a));
        p.rotation["LeftArm"] = axisAngle(Vec3{0.0f, 0.0f, 1.0f}, 3.0f * std::sin(a));
        p.rotation["RightArm"] = axisAngle(Vec3{0.0f, 0.0f, 1.0f}, -3.0f * std::sin(a));
    });
    b.finish("Maniqui");
    return std::move(b.model);
}

bool writeCreatureModel(const std::filesystem::path& file, const Uuid& uuid, const std::string& name,
                        const asset::ModelData& model, std::string* error) {
    // Como un personaje importado: una sola pieza animada con su esqueleto.
    assets::ModelNode root;
    root.name = name;
    root.part = 0;
    return assets::writeGeneratedModel(file, uuid, name, {root}, {model}, error, true);
}

}  // namespace cramion::editor
