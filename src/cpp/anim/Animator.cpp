#include "anim/Animator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cramion::anim {

using core::Mat4;
using core::Quat;
using core::Vec3;
using core::Vec4;

namespace {

// Posicion de `time` entre dos claves: indice de la primera y fraccion hasta
// la siguiente. Busqueda binaria, asi da igual cuantas claves tenga la pista.
template <typename Key>
std::pair<std::size_t, float> locate(const std::vector<Key>& keys, float time) {
    if (keys.size() == 1 || time <= keys.front().time) {
        return {0, 0.0f};
    }
    if (time >= keys.back().time) {
        return {keys.size() - 1, 0.0f};
    }

    const auto next = std::upper_bound(keys.begin(), keys.end(), time,
                                       [](float t, const Key& key) { return t < key.time; });
    const auto index = static_cast<std::size_t>(std::distance(keys.begin(), next)) - 1;

    const float span = keys[index + 1].time - keys[index].time;
    const float fraction = (span > 0.0f) ? (time - keys[index].time) / span : 0.0f;
    return {index, fraction};
}

Vec3 sampleVector(const std::vector<asset::VectorKey>& keys, float time) {
    const auto [index, fraction] = locate(keys, time);
    if (fraction <= 0.0f) {
        return keys[index].value;
    }
    return core::lerp(keys[index].value, keys[index + 1].value, fraction);
}

Quat sampleRotation(const std::vector<asset::QuatKey>& keys, float time) {
    const auto [index, fraction] = locate(keys, time);
    if (fraction <= 0.0f) {
        return keys[index].value;
    }
    return core::slerp(keys[index].value, keys[index + 1].value, fraction);
}

// Descompone la transformacion de reposo de un nodo (sin cizalla) para poder
// reemplazar solo las componentes que anima una pista.
void decompose(const Mat4& m, Vec3& translation, Quat& rotation, Vec3& scaling) {
    translation = Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};

    const Vec3 column0{m.m[0][0], m.m[0][1], m.m[0][2]};
    const Vec3 column1{m.m[1][0], m.m[1][1], m.m[1][2]};
    const Vec3 column2{m.m[2][0], m.m[2][1], m.m[2][2]};
    scaling = Vec3{core::length(column0), core::length(column1), core::length(column2)};

    const Vec3 r0 = column0 * (1.0f / std::max(scaling.x, 1e-8f));
    const Vec3 r1 = column1 * (1.0f / std::max(scaling.y, 1e-8f));
    const Vec3 r2 = column2 * (1.0f / std::max(scaling.z, 1e-8f));

    // Matriz de rotacion -> cuaternion (metodo de Shepperd, estable).
    const float trace = r0.x + r1.y + r2.z;
    Quat q{};
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(r1.z - r2.y) / s, (r2.x - r0.z) / s, (r0.y - r1.x) / s, 0.25f * s};
    } else if (r0.x > r1.y && r0.x > r2.z) {
        const float s = std::sqrt(1.0f + r0.x - r1.y - r2.z) * 2.0f;
        q = {0.25f * s, (r1.x + r0.y) / s, (r2.x + r0.z) / s, (r1.z - r2.y) / s};
    } else if (r1.y > r2.z) {
        const float s = std::sqrt(1.0f + r1.y - r0.x - r2.z) * 2.0f;
        q = {(r1.x + r0.y) / s, 0.25f * s, (r2.y + r1.z) / s, (r2.x - r0.z) / s};
    } else {
        const float s = std::sqrt(1.0f + r2.z - r0.x - r1.y) * 2.0f;
        q = {(r2.x + r0.z) / s, (r2.y + r1.z) / s, 0.25f * s, (r0.y - r1.x) / s};
    }
    rotation = core::normalize(q);
}

}  // namespace

Animator::Animator(const asset::ModelData& model) : model_(&model) {
    local_.resize(model.nodes.size(), Mat4::identity());
    global_.resize(model.nodes.size(), Mat4::identity());
    bone_matrices_.resize(model.bones.size(), Mat4::identity());
    node_channel_.assign(model.nodes.size(), -1);
    evaluate();
}

void Animator::play(std::int32_t clip, bool loop) {
    if (model_ == nullptr) {
        return;
    }

    clip_ = (clip >= 0 && static_cast<std::size_t>(clip) < model_->animations.size()) ? clip : -1;
    loop_ = loop;
    time_ = 0.0f;

    node_channel_.assign(model_->nodes.size(), -1);
    if (clip_ >= 0) {
        const asset::AnimationClip& animation = model_->animations[static_cast<std::size_t>(clip_)];
        for (std::size_t c = 0; c < animation.channels.size(); ++c) {
            node_channel_[static_cast<std::size_t>(animation.channels[c].node)] =
                static_cast<std::int32_t>(c);
        }
    }

    evaluate();
}

void Animator::setTime(float seconds) {
    time_ = seconds;
    evaluate();
}

void Animator::update(float delta_seconds) {
    if (model_ == nullptr) {
        return;
    }

    if (clip_ >= 0) {
        const float duration = model_->animations[static_cast<std::size_t>(clip_)].duration;
        time_ += delta_seconds * speed_;
        if (duration > 0.0f) {
            time_ = loop_ ? std::fmod(time_, duration) : std::min(time_, duration);
            if (time_ < 0.0f) {
                time_ += duration;
            }
        }
    }

    evaluate();
}

void Animator::evaluate() {
    if (model_ == nullptr) {
        return;
    }

    const asset::AnimationClip* animation =
        (clip_ >= 0) ? &model_->animations[static_cast<std::size_t>(clip_)] : nullptr;

    // --- 1 y 2) Pose local y global, de padres a hijos ---
    for (std::size_t i = 0; i < model_->nodes.size(); ++i) {
        const asset::Node& node = model_->nodes[i];

        const std::int32_t channel_index = node_channel_[i];
        if (animation != nullptr && channel_index >= 0) {
            const asset::AnimationChannel& channel =
                animation->channels[static_cast<std::size_t>(channel_index)];

            Vec3 translation;
            Quat rotation;
            Vec3 scaling;
            decompose(node.local, translation, rotation, scaling);

            if (!channel.positions.empty()) {
                translation = sampleVector(channel.positions, time_);
            }
            if (!channel.rotations.empty()) {
                rotation = sampleRotation(channel.rotations, time_);
            }
            if (!channel.scales.empty()) {
                scaling = sampleVector(channel.scales, time_);
            }
            local_[i] = core::composeTrs(translation, rotation, scaling);
        } else {
            local_[i] = node.local;
        }

        global_[i] = (node.parent >= 0) ? global_[static_cast<std::size_t>(node.parent)] * local_[i]
                                        : local_[i];
    }

    // --- 3) Matriz final de cada hueso ---
    for (std::size_t b = 0; b < model_->bones.size(); ++b) {
        const asset::Bone& bone = model_->bones[b];
        bone_matrices_[b] = (bone.node >= 0)
                                ? global_[static_cast<std::size_t>(bone.node)] * bone.offset
                                : bone.offset;
    }
}

Aabb skinnedBounds(const asset::ModelData& model, const std::vector<Mat4>& bones) {
    constexpr float kInf = std::numeric_limits<float>::max();
    Aabb box{Vec3{kInf}, Vec3{-kInf}};

    for (const asset::SkinnedVertex& vertex : model.vertices) {
        const Vec4 position{vertex.position, 1.0f};
        Vec3 skinned{};
        for (std::uint32_t k = 0; k < asset::kMaxBoneInfluences; ++k) {
            if (vertex.weights[k] <= 0.0f || vertex.joints[k] >= bones.size()) {
                continue;
            }
            const Vec4 p = bones[vertex.joints[k]] * position;
            skinned += Vec3{p.x, p.y, p.z} * vertex.weights[k];
        }

        box.min = Vec3{std::min(box.min.x, skinned.x), std::min(box.min.y, skinned.y),
                       std::min(box.min.z, skinned.z)};
        box.max = Vec3{std::max(box.max.x, skinned.x), std::max(box.max.y, skinned.y),
                       std::max(box.max.z, skinned.z)};
    }

    return box;
}

}  // namespace cramion::anim
