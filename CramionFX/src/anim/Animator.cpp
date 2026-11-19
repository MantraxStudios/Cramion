#include "CramionFX/anim/Animator.h"

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

// --- Cuaterniones y root motion ---------------------------------------------------

Quat qmul(const Quat& a, const Quat& b) {
    return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat qconj(const Quat& q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

Quat yawQuat(float angle) { return Quat{0.0f, std::sin(angle * 0.5f), 0.0f, std::cos(angle * 0.5f)}; }

float wrapAngle(float a) {
    constexpr float kTwoPi = 6.28318530718f;
    a = std::fmod(a + core::kPi, kTwoPi);
    if (a < 0.0f) a += kTwoPi;
    return a - core::kPi;
}

// Angulo del giro sobre +Y que contiene q (descomposicion swing-twist).
float twistYaw(const Quat& q) {
    if (std::abs(q.y) < 1e-8f && std::abs(q.w) < 1e-8f) return 0.0f;
    return wrapAngle(2.0f * std::atan2(q.y, q.w));
}

Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    const Vec4 r = m * Vec4{p, 1.0f};
    return Vec3{r.x, r.y, r.z};
}

// Lo que hace falta para pasar la raiz de su padre al espacio del modelo: la
// global en reposo del padre (los padres de la raiz no suelen animarse: el
// nodo "Armature" con la escala del FBX) y la pose de reposo de la raiz.
struct RootFrame {
    Mat4 parent = Mat4::identity();
    Mat4 parent_inv = Mat4::identity();
    Quat parent_rot{};
    Vec3 rest_t{};
    Quat rest_r{};
};

RootFrame rootFrame(const asset::ModelData& model, std::int32_t node) {
    RootFrame f;
    Mat4 parent = Mat4::identity();
    std::int32_t p = model.nodes[static_cast<std::size_t>(node)].parent;
    // De la raiz hacia abajo: global(padre) = global(abuelo) * local(padre).
    std::vector<std::int32_t> chain;
    for (int guard = 0; p >= 0 && static_cast<std::size_t>(p) < model.nodes.size() && guard < 256; ++guard) {
        chain.push_back(p);
        p = model.nodes[static_cast<std::size_t>(p)].parent;
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) parent = parent * model.nodes[static_cast<std::size_t>(*it)].local;
    f.parent = parent;
    f.parent_inv = core::inverse(parent);
    Vec3 t, s;
    decompose(parent, t, f.parent_rot, s);
    decompose(model.nodes[static_cast<std::size_t>(node)].local, f.rest_t, f.rest_r, s);
    return f;
}

// Posicion (modelo) y giro de la raiz en un instante del clip.
void sampleRootModel(const RootFrame& f, const asset::AnimationChannel& channel, float time, Vec3& position, Quat& rotation) {
    const Vec3 t = channel.positions.empty() ? f.rest_t : sampleVector(channel.positions, time);
    const Quat r = channel.rotations.empty() ? f.rest_r : sampleRotation(channel.rotations, time);
    position = transformPoint(f.parent, t);
    rotation = qmul(f.parent_rot, r);
}

// Deja la raiz como en el primer fotograma del clip en XZ y sin el giro en Y
// que lleva desde el: el personaje anima en el sitio.
void stripRootPose(const RootFrame& f, const asset::AnimationChannel& channel, Vec3& t, Quat& r) {
    Vec3 p0;
    Quat q0;
    sampleRootModel(f, channel, 0.0f, p0, q0);
    const Vec3 p = transformPoint(f.parent, t);
    const Quat q = qmul(f.parent_rot, r);
    const float yaw = twistYaw(qmul(q, qconj(q0)));
    const Quat q2 = qmul(yawQuat(-yaw), q);
    r = core::normalize(qmul(qconj(f.parent_rot), q2));
    t = transformPoint(f.parent_inv, Vec3{p0.x, p.y, p0.z});
}

// Movimiento entre dos instantes del mismo ciclo (a <= b o al reves).
RootMotion rootSegment(const RootFrame& f, const asset::AnimationChannel& channel, float a, float b) {
    Vec3 p0, pa, pb;
    Quat q0, qa, qb;
    sampleRootModel(f, channel, 0.0f, p0, q0);
    sampleRootModel(f, channel, a, pa, qa);
    sampleRootModel(f, channel, b, pb, qb);
    const float ya = twistYaw(qmul(qa, qconj(q0)));
    const float yb = twistYaw(qmul(qb, qconj(q0)));
    Vec3 d = pb - pa;
    d.y = 0.0f;
    return RootMotion{rotateAboutY(d, -ya), wrapAngle(yb - ya)};
}

// Hacia delante desde `start` durante `length` segundos (> 0).
RootMotion rootForward(const RootFrame& f, const asset::AnimationChannel& channel, float duration, float start,
                       float length, bool loop) {
    if (!loop || duration <= 1e-5f) {
        const float d = std::max(duration, 0.0f);
        return rootSegment(f, channel, std::clamp(start, 0.0f, d), std::clamp(start + length, 0.0f, d));
    }
    RootMotion acc{};
    float cur = start;
    const float end = start + std::min(length, duration * 8.0f);  // como mucho 8 vueltas por frame
    for (int guard = 0; cur < end - 1e-6f && guard < 32; ++guard) {
        const float cycle = std::floor(cur / duration);
        const float la = cur - cycle * duration;
        const float seg_end = std::min(end, (cycle + 1.0f) * duration);
        const float lb = std::min(seg_end - cycle * duration, duration);
        const RootMotion seg = rootSegment(f, channel, la, lb);
        acc.position = acc.position + rotateAboutY(seg.position, acc.yaw);
        acc.yaw += seg.yaw;
        cur = seg_end;
    }
    return acc;
}

RootMotion channelRootMotion(const RootFrame& f, const asset::AnimationChannel& channel, float duration, float time,
                             float delta, bool loop) {
    if (delta == 0.0f) return {};
    if (delta > 0.0f) return rootForward(f, channel, duration, time - delta, delta, loop);
    // Hacia atras (velocidad negativa): lo contrario de ir de `time` a `time - delta`.
    const RootMotion fwd = rootForward(f, channel, duration, time, -delta, loop);
    return RootMotion{rotateAboutY(Vec3{-fwd.position.x, 0.0f, -fwd.position.z}, -fwd.yaw), -fwd.yaw};
}

}  // namespace

Animator::Animator(const asset::ModelData& model) : model_(&model) {
    local_.resize(model.nodes.size(), Mat4::identity());
    global_.resize(model.nodes.size(), Mat4::identity());
    bone_matrices_.resize(model.bones.size(), Mat4::identity());
    node_channel_.assign(model.nodes.size(), -1);
    rest_translation_.resize(model.nodes.size());
    rest_rotation_.resize(model.nodes.size());
    rest_scale_.resize(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        decompose(model.nodes[i].local, rest_translation_[i], rest_rotation_[i], rest_scale_[i]);
    }
    evaluate();
}

float Animator::gatherActive(const ClipSample* samples, std::size_t count) {
    const std::size_t node_count = model_->nodes.size();
    if (clip_channels_.size() < model_->animations.size()) {
        clip_channels_.resize(model_->animations.size());
    }
    // Solo las muestras que cuentan (clip valido o reposo, peso > 0). La
    // lista es del animador y se reutiliza (antes, una reserva por frame).
    std::vector<BlendActive>& active = blend_active_;
    active.clear();
    active_delta_.clear();
    active_loop_.clear();
    float total = 0.0f;
    for (std::size_t k = 0; k < count; ++k) {
        const ClipSample& s = samples[k];
        if (!(s.weight > 1e-5f)) continue;
        BlendActive a{nullptr, nullptr, s.time, s.weight};
        if (s.clip >= 0 && static_cast<std::size_t>(s.clip) < model_->animations.size()) {
            const asset::AnimationClip& clip = model_->animations[static_cast<std::size_t>(s.clip)];
            std::vector<std::int32_t>& map = clip_channels_[static_cast<std::size_t>(s.clip)];
            if (map.size() != node_count) {
                map.assign(node_count, -1);
                for (std::size_t c = 0; c < clip.channels.size(); ++c) {
                    const std::int32_t node = clip.channels[c].node;
                    if (node >= 0 && static_cast<std::size_t>(node) < node_count) map[static_cast<std::size_t>(node)] = static_cast<std::int32_t>(c);
                }
            }
            a.clip = &clip;
            a.channels = &map;
        }
        active.push_back(a);
        if (root_node_ >= 0) {
            active_delta_.push_back(s.delta);
            active_loop_.push_back(s.loop ? 1 : 0);
        }
        total += s.weight;
    }
    return total;
}

void Animator::blendNode(std::size_t i, float total, Vec3& translation, Quat& rotation_out, Vec3& scaling) const {
    translation = Vec3{};
    scaling = Vec3{};
    Quat rotation{0.0f, 0.0f, 0.0f, 0.0f};
    Quat first{};
    bool have_first = false;
    const bool root = root_node_ >= 0 && static_cast<std::size_t>(root_node_) == i;
    for (const BlendActive& a : blend_active_) {
        Vec3 t = rest_translation_[i];
        Quat r = rest_rotation_[i];
        Vec3 sc = rest_scale_[i];
        const std::int32_t channel_index = a.channels != nullptr ? (*a.channels)[i] : -1;
        if (channel_index >= 0) {
            const asset::AnimationChannel& channel = a.clip->channels[static_cast<std::size_t>(channel_index)];
            if (!channel.positions.empty()) t = sampleVector(channel.positions, a.time);
            if (!channel.rotations.empty()) r = sampleRotation(channel.rotations, a.time);
            if (!channel.scales.empty()) sc = sampleVector(channel.scales, a.time);
            // Root motion: la raiz se queda en el sitio (lo que avanza y gira lo
            // aplica quien lo pidio).
            if (root) stripRoot(*a.clip, channel_index, t, r);
        }
        const float w = a.weight / total;
        translation = translation + t * w;
        scaling = scaling + sc * w;
        // Mismo hemisferio que la primera: si no, la mezcla da la vuelta larga.
        if (!have_first) {
            first = r;
            have_first = true;
        } else if (first.x * r.x + first.y * r.y + first.z * r.z + first.w * r.w < 0.0f) {
            r = Quat{-r.x, -r.y, -r.z, -r.w};
        }
        rotation = Quat{rotation.x + r.x * w, rotation.y + r.y * w, rotation.z + r.z * w, rotation.w + r.w * w};
    }
    rotation_out = core::normalize(rotation);
}

void Animator::evaluateBlend(const ClipSample* samples, std::size_t count) {
    if (model_ == nullptr) {
        return;
    }
    const std::size_t node_count = model_->nodes.size();
    const float total = gatherActive(samples, count);
    const bool empty = blend_active_.empty();
    for (std::size_t i = 0; i < node_count; ++i) {
        const asset::Node& node = model_->nodes[i];
        if (empty) {
            local_[i] = node.local;
        } else {
            Vec3 translation;
            Quat rotation;
            Vec3 scaling;
            blendNode(i, total, translation, rotation, scaling);
            local_[i] = core::composeTrs(translation, rotation, scaling);
        }
        global_[i] = (node.parent >= 0) ? global_[static_cast<std::size_t>(node.parent)] * local_[i] : local_[i];
    }
    root_motion_ = root_node_ >= 0 && !empty ? activeRootMotion(total) : RootMotion{};
    updateBones();
}

void Animator::samplePose(const ClipSample* samples, std::size_t count, LocalPose& out, RootMotion* motion) {
    if (model_ == nullptr) return;
    const std::size_t node_count = model_->nodes.size();
    out.resize(node_count);
    const float total = gatherActive(samples, count);
    if (blend_active_.empty()) {
        restPose(out);
        if (motion != nullptr) *motion = RootMotion{};
        return;
    }
    for (std::size_t i = 0; i < node_count; ++i) {
        blendNode(i, total, out.translation[i], out.rotation[i], out.scale[i]);
    }
    if (motion != nullptr) *motion = root_node_ >= 0 ? activeRootMotion(total) : RootMotion{};
}

void Animator::restPose(LocalPose& out) const {
    if (model_ == nullptr) return;
    out.resize(model_->nodes.size());
    for (std::size_t i = 0; i < model_->nodes.size() && i < rest_translation_.size(); ++i) {
        out.translation[i] = rest_translation_[i];
        out.rotation[i] = rest_rotation_[i];
        out.scale[i] = rest_scale_[i];
    }
}

void Animator::applyPose(const LocalPose& pose) {
    if (model_ == nullptr || pose.size() < model_->nodes.size()) return;
    for (std::size_t i = 0; i < model_->nodes.size(); ++i) {
        const asset::Node& node = model_->nodes[i];
        local_[i] = core::composeTrs(pose.translation[i], pose.rotation[i], pose.scale[i]);
        global_[i] = (node.parent >= 0) ? global_[static_cast<std::size_t>(node.parent)] * local_[i] : local_[i];
    }
    updateBones();
}

void Animator::evaluateLayered(const LayeredPosePlan& plan) {
    if (model_ == nullptr) return;
    RootMotion motion{};
    samplePose(plan.base.data(), plan.base.size(), layer_base_, &motion);
    const std::size_t root = root_node_ >= 0 ? static_cast<std::size_t>(root_node_) : 0;
    for (std::size_t l = 0; l < plan.layer_count && l < plan.layers.size(); ++l) {
        const PoseLayerJob& job = plan.layers[l];
        const float weight = std::clamp(job.weight, 0.0f, 1.0f);
        if (weight <= 1e-5f || job.samples.empty()) continue;
        const std::vector<float>* mask = job.mask ? job.mask.get() : nullptr;
        if (mask != nullptr && mask->size() < model_->nodes.size()) mask = nullptr;  // de otro esqueleto
        if (job.mode == LayerBlendMode::Additive) {
            samplePose(job.samples.data(), job.samples.size(), layer_pose_, nullptr);
            if (!job.reference.empty()) {
                samplePose(job.reference.data(), job.reference.size(), layer_ref_, nullptr);
            } else {
                // Referencia = el primer fotograma de cada clip de la capa.
                thread_local std::vector<ClipSample> first;
                first.assign(job.samples.begin(), job.samples.end());
                for (ClipSample& s : first) {
                    s.time = 0.0f;
                    s.delta = 0.0f;
                }
                samplePose(first.data(), first.size(), layer_ref_, nullptr);
            }
            makeAdditivePose(layer_pose_, layer_ref_);
            applyAdditivePose(layer_base_, layer_pose_, weight, mask);
            continue;
        }
        RootMotion layer_motion{};
        samplePose(job.samples.data(), job.samples.size(), layer_pose_, job.root_motion ? &layer_motion : nullptr);
        blendPoseOverride(layer_base_, layer_pose_, weight, mask);
        if (job.root_motion && root_node_ >= 0) {
            const float w = weight * (mask != nullptr ? std::clamp((*mask)[root], 0.0f, 1.0f) : 1.0f);
            motion.position = core::lerp(motion.position, layer_motion.position, w);
            motion.yaw = motion.yaw + (layer_motion.yaw - motion.yaw) * w;
        }
    }
    applyPose(layer_base_);
    root_motion_ = root_node_ >= 0 ? motion : RootMotion{};
}

void Animator::setRootMotionNode(std::int32_t node) {
    if (model_ == nullptr || node < 0 || static_cast<std::size_t>(node) >= model_->nodes.size()) {
        root_node_ = -1;
        root_motion_ = RootMotion{};
        return;
    }
    if (node == root_node_) return;
    root_node_ = node;
    root_motion_ = RootMotion{};
    const RootFrame frame = rootFrame(*model_, node);
    root_parent_ = frame.parent;
    root_parent_inv_ = frame.parent_inv;
    root_parent_rot_ = frame.parent_rot;
}

void Animator::stripRoot(const asset::AnimationClip& clip, std::int32_t channel_index, Vec3& t, Quat& r) const {
    const asset::AnimationChannel& channel = clip.channels[static_cast<std::size_t>(channel_index)];
    const std::size_t i = static_cast<std::size_t>(root_node_);
    RootFrame frame{root_parent_, root_parent_inv_, root_parent_rot_, rest_translation_[i], rest_rotation_[i]};
    stripRootPose(frame, channel, t, r);
}

RootMotion Animator::activeRootMotion(float total) const {
    RootMotion sum{};
    if (total <= 0.0f || root_node_ < 0) return sum;
    for (std::size_t k = 0; k < blend_active_.size() && k < active_delta_.size(); ++k) {
        const BlendActive& a = blend_active_[k];
        if (a.clip == nullptr || active_delta_[k] == 0.0f) continue;
        const std::int32_t channel_index = a.channels != nullptr ? (*a.channels)[static_cast<std::size_t>(root_node_)] : -1;
        if (channel_index < 0) continue;
        const std::size_t i = static_cast<std::size_t>(root_node_);
        RootFrame frame{root_parent_, root_parent_inv_, root_parent_rot_, rest_translation_[i], rest_rotation_[i]};
        const RootMotion m = channelRootMotion(frame, a.clip->channels[static_cast<std::size_t>(channel_index)],
                                               a.clip->duration, a.time, active_delta_[k], active_loop_[k] != 0);
        const float w = a.weight / total;
        sum.position = sum.position + m.position * w;
        sum.yaw += m.yaw * w;
    }
    return sum;
}

// --- Funciones libres -----------------------------------------------------------

Vec3 rotateAboutY(const Vec3& v, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return Vec3{v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

RootMotion clipRootMotion(const asset::ModelData& model, const asset::AnimationClip& clip, std::int32_t node,
                          float time, float delta, bool loop) {
    if (node < 0 || static_cast<std::size_t>(node) >= model.nodes.size()) return {};
    for (const asset::AnimationChannel& channel : clip.channels) {
        if (channel.node != node) continue;
        const RootFrame frame = rootFrame(model, node);
        return channelRootMotion(frame, channel, clip.duration, time, delta, loop);
    }
    return {};
}

void blendPoseOverride(LocalPose& base, const LocalPose& layer, float weight, const std::vector<float>* mask) {
    const std::size_t n = std::min(base.size(), layer.size());
    for (std::size_t i = 0; i < n; ++i) {
        const float w = weight * (mask != nullptr && i < mask->size() ? (*mask)[i] : 1.0f);
        if (w <= 1e-5f) continue;
        if (w >= 1.0f - 1e-5f) {
            base.translation[i] = layer.translation[i];
            base.rotation[i] = layer.rotation[i];
            base.scale[i] = layer.scale[i];
            continue;
        }
        base.translation[i] = core::lerp(base.translation[i], layer.translation[i], w);
        base.scale[i] = core::lerp(base.scale[i], layer.scale[i], w);
        base.rotation[i] = core::slerp(base.rotation[i], layer.rotation[i], w);
    }
}

void makeAdditivePose(LocalPose& pose, const LocalPose& reference) {
    const std::size_t n = std::min(pose.size(), reference.size());
    for (std::size_t i = 0; i < n; ++i) {
        pose.translation[i] = pose.translation[i] - reference.translation[i];
        pose.rotation[i] = core::normalize(qmul(qconj(reference.rotation[i]), pose.rotation[i]));
        const Vec3& rs = reference.scale[i];
        const Vec3& s = pose.scale[i];
        pose.scale[i] = Vec3{std::abs(rs.x) > 1e-8f ? s.x / rs.x : 1.0f, std::abs(rs.y) > 1e-8f ? s.y / rs.y : 1.0f,
                             std::abs(rs.z) > 1e-8f ? s.z / rs.z : 1.0f};
    }
}

void applyAdditivePose(LocalPose& base, const LocalPose& delta, float weight, const std::vector<float>* mask) {
    const std::size_t n = std::min(base.size(), delta.size());
    for (std::size_t i = 0; i < n; ++i) {
        const float w = weight * (mask != nullptr && i < mask->size() ? (*mask)[i] : 1.0f);
        if (w <= 1e-5f) continue;
        base.translation[i] = base.translation[i] + delta.translation[i] * w;
        const Quat d = w >= 1.0f - 1e-5f ? delta.rotation[i] : core::slerp(Quat{}, delta.rotation[i], w);
        base.rotation[i] = core::normalize(qmul(base.rotation[i], d));
        const Vec3 one{1.0f, 1.0f, 1.0f};
        base.scale[i] = base.scale[i] * core::lerp(one, delta.scale[i], w);
    }
}

void collectClipEvents(const asset::AnimationClip& clip, float time, float delta, bool loop, bool include_start,
                       std::vector<std::uint32_t>& out) {
    if (clip.events.empty()) return;
    const float duration = clip.duration;
    constexpr float kEps = 1e-5f;
    const float a = time - delta;  // de donde viene
    const float b = time;          // a donde llega
    for (std::size_t k = 0; k < clip.events.size(); ++k) {
        const float e = clip.events[k].time;
        bool fire = false;
        if (loop && duration > kEps) {
            if (delta > 0.0f) {
                // Hay alguna vuelta n con a < e + n*d <= b?
                const float n = std::floor((a - e) / duration) + 1.0f;
                fire = e + n * duration <= b + kEps;
            } else if (delta < 0.0f) {
                // Hacia atras: b <= e + n*d < a.
                const float n = std::ceil((b - e) / duration);
                fire = e + n * duration < a - kEps;
            }
            if (!fire && include_start) {
                const float m = std::fmod(a - e, duration);
                fire = std::abs(m) <= kEps || std::abs(std::abs(m) - duration) <= kEps;
            }
        } else {
            const float lo = std::clamp(std::min(a, b), 0.0f, std::max(duration, 0.0f));
            const float hi = std::clamp(std::max(a, b), 0.0f, std::max(duration, 0.0f));
            if (delta > 0.0f) fire = e > lo + kEps && e <= hi + kEps;
            else if (delta < 0.0f) fire = e >= lo - kEps && e < hi - kEps;
            const float start = std::clamp(a, 0.0f, std::max(duration, 0.0f));
            if (!fire && include_start) fire = std::abs(e - start) <= kEps;
        }
        if (fire) out.push_back(static_cast<std::uint32_t>(k));
    }
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
    advance(delta_seconds);
    evaluate();
}

void Animator::advance(float delta_seconds) {
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
}

void Animator::evaluate() {
    if (model_ == nullptr) {
        return;
    }

    const asset::AnimationClip* animation =
        (clip_ >= 0) ? &model_->animations[static_cast<std::size_t>(clip_)] : nullptr;

    // La pose de reposo ya descompuesta (constructor): antes se descomponia
    // node.local de cada nodo animado en cada frame (raices y normalizar).
    const bool rest_cached = rest_translation_.size() == model_->nodes.size();

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
            if (rest_cached) {
                translation = rest_translation_[i];
                rotation = rest_rotation_[i];
                scaling = rest_scale_[i];
            } else {
                decompose(node.local, translation, rotation, scaling);
            }

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

    updateBones();
}

void Animator::updateBones() {
    if (model_ == nullptr) return;
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

namespace cramion::asset {

float ClipCurve::sample(float time) const {
    if (keys.empty()) return 0.0f;
    if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    const auto next = std::upper_bound(keys.begin(), keys.end(), time,
                                       [](float t, const CurveKey& key) { return t < key.time; });
    const auto& k1 = *next;
    const auto& k0 = *(next - 1);
    const float span = k1.time - k0.time;
    const float f = span > 0.0f ? (time - k0.time) / span : 0.0f;
    return k0.value + (k1.value - k0.value) * f;
}

}  // namespace cramion::asset
