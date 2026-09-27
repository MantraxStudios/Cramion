// RenderSync: esqueletos (huesos movidos a mano, sockets), phys bones y la
// parte de animacion del ragdoll. Ver RenderSync.h.

#include "CramionCore/ecs/RenderSync.h"

#include "CramionCore/anim/Creature.h"
#include "CramionCore/anim/IK.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/Rigging.h"
#include "CramionCore/physics/Ragdoll.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <cmath>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace cramion::ecs {

using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

// La pieza y los antepasados que son "el mismo modelo": se para en un Bone
// Socket (lo que cuelga de un hueso es un accesorio: una espada, un
// sombrero) y en un antepasado que dibuja otro modelo.
std::vector<Entity> sameModelChain(Entity e) {
    std::vector<Entity> chain{e};
    const MeshRenderer* own = e.tryGet<MeshRenderer>();
    Entity below = e;
    for (Entity a = e.parent(); a.valid() && chain.size() < 32; a = a.parent()) {
        if (below.has<BoneSocket>()) break;
        if (const MeshRenderer* mr = a.tryGet<MeshRenderer>();
            mr != nullptr && own != nullptr && !(mr->model.uuid == own->model.uuid)) {
            break;
        }
        chain.push_back(a);
        below = a;
    }
    return chain;
}

template <typename T>
T* findUp(const std::vector<Entity>& chain, Entity* owner) {
    for (const Entity& e : chain) {
        if (T* c = e.tryGet<T>()) {
            if (owner != nullptr) *owner = e;
            return c;
        }
    }
    return nullptr;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}
std::uint64_t mixString(std::uint64_t h, const std::string& s) { return mix(h, std::hash<std::string>{}(s)); }
std::uint64_t mixFloat(std::uint64_t h, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return mix(h, bits);
}

Mat4 withoutScale(const Mat4& m) {
    Vec3 t{};
    Quat r{};
    Vec3 s{};
    decomposeMatrix(m, t, r, s);
    return core::composeTrs(t, r, Vec3{1.0f, 1.0f, 1.0f});
}

std::vector<std::string> splitNames(const std::string& list) {
    std::vector<std::string> out;
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const auto b = item.find_first_not_of(" \t");
        const auto e = item.find_last_not_of(" \t");
        if (b != std::string::npos) out.push_back(item.substr(b, e - b + 1));
    }
    return out;
}

}  // namespace

// --- Que componentes de esqueleto afectan a una pieza -----------------------------

RenderSync::RigComponents RenderSync::gatherRig(Entity e) {
    RigComponents rig;
    Entity owner;
    const auto consider = [&](Entity o) {
        if (o.valid() && o != e) {
            // La pose la calcula la primera pieza y las demas la copian: la
            // clave es el mas alto de los duenos que no es la propia pieza.
            if (rig.share == entt::null || o.isAncestorOf(e.world()->wrap(rig.share))) rig.share = o.handle();
        }
    };
    const std::vector<Entity> chain = sameModelChain(e);
    rig.ik = findUp<InverseKinematics>(chain, &owner);
    if (rig.ik != nullptr) consider(owner);
    rig.proc = findUp<ProceduralAnimation>(chain, &owner);
    if (rig.proc != nullptr) consider(owner);
    rig.skeleton = findUp<Skeleton>(chain, &owner);
    if (rig.skeleton != nullptr) consider(owner);
    rig.physbones = findUp<PhysBones>(chain, &owner);
    if (rig.physbones != nullptr) consider(owner);
    rig.ragdoll = findUp<Ragdoll>(chain, &owner);
    if (rig.ragdoll != nullptr) {
        consider(owner);
        rig.ragdoll_owner = owner.handle();
    }
    rig.drive = drive_sockets_.contains(e.handle());
    return rig;
}

bool RenderSync::copySharedPose(const RigComponents& rig, anim::Animator& animator, const asset::ModelData& data) {
    if (rig.share == entt::null) return false;
    const auto it = shared_poses_.find(rig.share);
    if (it == shared_poses_.end() || it->second.frame != frame_ || it->second.locals.size() != data.nodes.size() ||
        animator.locals().size() != data.nodes.size()) {
        return false;
    }
    animator.locals() = it->second.locals;
    ik::Pose pose{&data.nodes, &animator.locals(), &animator.globals()};
    ik::recomputeGlobals(pose, 0);
    animator.updateBones();
    return true;
}

void RenderSync::storeSharedPose(const RigComponents& rig, anim::Animator& animator) {
    if (rig.share == entt::null) return;
    SharedPose& shared = shared_poses_[rig.share];
    shared.frame = frame_;
    shared.locals = animator.locals();
}

// --- Huesos movidos a mano (Skeleton) --------------------------------------------

void RenderSync::applyBoneOverrides(const Skeleton& skeleton, anim::Animator& animator, const asset::ModelData& data) {
    if (skeleton.bones.empty() || animator.locals().size() != data.nodes.size()) return;
    ik::Pose pose{&data.nodes, &animator.locals(), &animator.globals()};
    int first = INT_MAX;
    for (const BoneOverride& o : skeleton.bones) {
        const int n = findBone(data, o.bone);
        if (n < 0 || o.weight <= 0.0f) continue;
        Mat4& local = animator.locals()[static_cast<std::size_t>(n)];
        Vec3 t{};
        Quat r{};
        Vec3 s{};
        decomposeMatrix(local, t, r, s);
        const float w = std::clamp(o.weight, 0.0f, 1.0f);
        t = t + o.position * w;
        r = core::normalize(quatMultiply(r, core::slerp(Quat{}, quatFromEulerDegrees(o.rotation), w)));
        s = s * core::lerp(Vec3{1.0f, 1.0f, 1.0f}, o.scale, w);
        local = core::composeTrs(t, r, s);
        first = std::min(first, n);
    }
    if (first != INT_MAX) ik::recomputeGlobals(pose, first);
}

// --- Sockets que mueven huesos ---------------------------------------------------

void RenderSync::applyDriveSockets(World& world, Entity entity, anim::Animator& animator, const asset::ModelData& data) {
    const auto it = drive_sockets_.find(entity.handle());
    if (it == drive_sockets_.end() || animator.locals().size() != data.nodes.size()) return;
    ik::Pose pose{&data.nodes, &animator.locals(), &animator.globals()};
    const Mat4 to_model = core::inverse(entity.worldMatrix());
    for (const entt::entity h : it->second) {
        const Entity s = world.wrap(h);
        if (!s.valid() || !s.activeInHierarchy()) continue;
        const BoneSocket* socket = s.tryGet<BoneSocket>();
        if (socket == nullptr || socket->mode != SocketMode::Drive || socket->weight <= 0.0f) continue;
        const int n = findBone(data, socket->bone);
        if (n < 0) continue;
        const Mat4 offset = core::composeTrs(socket->position, quatFromEulerDegrees(socket->rotation), Vec3{1.0f, 1.0f, 1.0f});
        const Mat4 bone_world = withoutScale(s.worldMatrix()) * core::inverse(offset);
        Vec3 t{};
        Quat r{};
        Vec3 sc{};
        decomposeMatrix(to_model * bone_world, t, r, sc);
        const float w = std::clamp(socket->weight, 0.0f, 1.0f);
        ik::setGlobalRotation(pose, n, core::slerp(ik::nodeRotation(pose, n), r, w));
        if (socket->drive_position) ik::translateGlobal(pose, n, (t - ik::nodePosition(pose, n)) * w);
    }
}

// --- Phys Bones -----------------------------------------------------------------

void RenderSync::applyPhysBones(World& world, Entity entity, const PhysBones& bones, anim::Animator& animator,
                                const asset::ModelData& data, float delta_seconds) {
    if (animator.locals().size() != data.nodes.size()) return;
    ik::Pose pose{&data.nodes, &animator.locals(), &animator.globals()};
    PhysBoneState& state = physbones_[entity.handle()];
    std::uint64_t signature = mix(0, data.nodes.size());
    for (const PhysBoneChain& c : bones.chains) {
        signature = mixString(signature, c.bone);
        signature = mixString(signature, c.ignore);
        signature = mix(signature, c.end_length > 0.0f ? 1 : 0);
    }
    if (state.signature != signature || state.chains.size() != bones.chains.size()) {
        state.signature = signature;
        state.chains.clear();
        for (const PhysBoneChain& c : bones.chains) {
            std::vector<int> ignore;
            for (const std::string& name : splitNames(c.ignore)) {
                const int n = findBone(data, name);
                if (n >= 0) ignore.push_back(n);
            }
            state.chains.push_back(physbone::makeChain(data.nodes, findBone(data, c.bone), ignore, c.end_length > 0.0f));
        }
    }

    // Colliders en el mundo.
    std::vector<physbone::Collider> colliders;
    const auto add_collider = [&](Entity ce) {
        if (!ce.valid() || !ce.activeInHierarchy()) return;
        const PhysBoneCollider* pc = ce.tryGet<PhysBoneCollider>();
        if (pc == nullptr) return;
        const Mat4& m = ce.worldMatrix();
        const float scale = maxAxisScale(m);
        physbone::Collider c;
        c.radius = pc->radius * scale;
        c.inside = pc->inside;
        switch (pc->shape) {
            case PhysBoneColliderShape::Sphere:
                c.shape = physbone::Collider::Shape::Sphere;
                c.a = transformPoint(m, pc->offset);
                break;
            case PhysBoneColliderShape::Capsule: {
                c.shape = physbone::Collider::Shape::Capsule;
                const float half = std::max(pc->height * 0.5f - pc->radius, 0.0f);
                c.a = transformPoint(m, pc->offset + Vec3{0.0f, half, 0.0f});
                c.b = transformPoint(m, pc->offset - Vec3{0.0f, half, 0.0f});
                break;
            }
            case PhysBoneColliderShape::Plane:
                c.shape = physbone::Collider::Shape::Plane;
                c.a = transformPoint(m, pc->offset);
                c.normal = core::normalize(transformDirection(m, Vec3{0.0f, 1.0f, 0.0f}));
                break;
        }
        colliders.push_back(c);
    };
    if (bones.colliders.empty()) {
        for (const entt::entity h : world.registry().view<PhysBoneCollider>()) add_collider(world.wrap(h));
    } else {
        for (const Uuid& id : bones.colliders) add_collider(world.find(id));
    }

    const Mat4& world_matrix = entity.worldMatrix();
    for (std::size_t i = 0; i < bones.chains.size() && i < state.chains.size(); ++i) {
        const PhysBoneChain& c = bones.chains[i];
        physbone::Settings s;
        s.pull = c.pull;
        s.spring = c.spring;
        s.stiffness = c.stiffness;
        s.gravity = c.gravity;
        s.gravity_falloff = c.gravity_falloff;
        s.immobile = c.immobile;
        s.max_angle = c.max_angle;
        s.radius = c.radius;
        s.radius_tip = c.radius_tip;
        s.end_length = c.end_length;
        s.collide = c.collide;
        physbone::update(pose, world_matrix, state.chains[i], s, colliders, delta_seconds);
    }
}

// --- Ragdoll: la animacion ---------------------------------------------------------

void RenderSync::applyRagdoll(Entity entity, Ragdoll& ragdoll, anim::Animator& animator, const asset::ModelData& data,
                              float delta_seconds) {
    if (animator.locals().size() != data.nodes.size()) return;
    if (!ragdoll.runtime.ptr) ragdoll.runtime.ptr = std::make_shared<RagdollRuntime>();
    RagdollRuntime& rt = *ragdoll.runtime.ptr;
    ik::Pose pose{&data.nodes, &animator.locals(), &animator.globals()};
    const Mat4& world_matrix = entity.worldMatrix();

    // Huesos (se rehacen si cambia la configuracion; nunca en plena caida).
    std::uint64_t signature = mix(mix(0, data.nodes.size()), reinterpret_cast<std::uintptr_t>(&data));
    signature = mixFloat(signature, ragdoll.mass);
    for (const RagdollBoneSetting& b : ragdoll.bones) {
        signature = mixString(signature, b.bone);
        signature = mixFloat(signature, b.radius);
        signature = mixFloat(signature, b.length);
        signature = mixFloat(signature, b.mass);
        signature = mixFloat(signature, b.swing);
        signature = mixFloat(signature, b.twist);
    }
    if (rt.signature != signature && !rt.simulating) {
        rt.signature = signature;
        rt.bones.clear();
        rt.pose_ready = false;
        rt.has_previous = false;
        rt.sim_ready = false;
        const float scale = std::max(maxAxisScale(world_matrix), 1e-6f);
        const std::vector<Mat4> rest = humanoid::restGlobals(data.nodes);
        const std::vector<creature::RagdollBone> automatic = creature::ragdollBones(data.nodes, rest);
        std::vector<RagdollRuntime::Bone> bones;
        std::vector<float> auto_length;  // modelo
        std::vector<int> tips;
        std::vector<float> weights;      // reparto de la masa
        std::vector<bool> fixed_mass;
        const auto auto_of = [&](int node) -> const creature::RagdollBone* {
            for (const creature::RagdollBone& a : automatic) {
                if (a.node == node) return &a;
            }
            return nullptr;
        };
        if (ragdoll.bones.empty()) {
            for (const creature::RagdollBone& a : automatic) {
                RagdollRuntime::Bone b;
                b.name = data.nodes[static_cast<std::size_t>(a.node)].name;
                b.node = a.node;
                b.radius = a.radius * scale;
                b.swing = a.swing;
                b.twist = a.twist;
                bones.push_back(b);
                auto_length.push_back(a.length);
                tips.push_back(a.tip);
                weights.push_back(a.mass);
                fixed_mass.push_back(false);
            }
        } else {
            std::vector<std::pair<int, const RagdollBoneSetting*>> list;
            for (const RagdollBoneSetting& s : ragdoll.bones) {
                const int n = findBone(data, s.bone);
                if (n >= 0) list.emplace_back(n, &s);
            }
            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            list.erase(std::unique(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first == b.first; }),
                       list.end());
            for (const auto& [n, s] : list) {
                const creature::RagdollBone* a = auto_of(n);
                RagdollRuntime::Bone b;
                b.name = data.nodes[static_cast<std::size_t>(n)].name;
                b.node = n;
                const float length_model = s->length > 0.0f ? s->length / scale
                                                            : (a != nullptr ? a->length
                                                                            : creature::boneLength(data.nodes, rest, n, 0.0f));
                b.radius = s->radius > 0.0f ? s->radius
                                            : (a != nullptr ? a->radius * scale : std::max(length_model * 0.2f, 0.01f / scale) * scale);
                b.swing = s->swing;
                b.twist = s->twist;
                b.mass = s->mass;
                bones.push_back(b);
                auto_length.push_back(length_model);
                tips.push_back(a != nullptr ? a->tip : -1);
                weights.push_back(b.radius * b.radius * std::max(length_model * scale, b.radius));
                fixed_mass.push_back(s->mass > 0.0f);
            }
        }
        // Padres: el antepasado mas cercano de la lista.
        for (std::size_t k = 0; k < bones.size(); ++k) {
            for (int a = data.nodes[static_cast<std::size_t>(bones[k].node)].parent; a >= 0 && bones[k].parent < 0;
                 a = data.nodes[static_cast<std::size_t>(a)].parent) {
                for (std::size_t j = 0; j < k; ++j) {
                    if (bones[j].node == a) {
                        bones[k].parent = static_cast<int>(j);
                        break;
                    }
                }
            }
        }
        // Masa: la fija se respeta, el resto se reparte.
        float fixed = 0.0f;
        float share = 0.0f;
        for (std::size_t k = 0; k < bones.size(); ++k) {
            if (fixed_mass[k]) fixed += bones[k].mass;
            else share += weights[k];
        }
        const float remaining = std::max(ragdoll.mass - fixed, ragdoll.mass * 0.1f);
        for (std::size_t k = 0; k < bones.size(); ++k) {
            if (!fixed_mass[k]) bones[k].mass = share > 0.0f ? remaining * weights[k] / share : remaining / static_cast<float>(bones.size());
            bones[k].mass = std::max(bones[k].mass, 0.05f);
        }
        rt.bones = std::move(bones);
        ragdoll_tips_[entity.handle()] = {std::move(tips), std::move(auto_length)};
    }
    if (rt.bones.empty()) return;

    // La pose animada, en el mundo (la fisica la usa al empezar a caer).
    const auto tips_it = ragdoll_tips_.find(entity.handle());
    for (std::size_t k = 0; k < rt.bones.size(); ++k) {
        RagdollRuntime::Bone& b = rt.bones[k];
        b.previous = b.world;
        b.world = world_matrix * animator.globals()[static_cast<std::size_t>(b.node)];
        const Vec3 origin{b.world.m[3][0], b.world.m[3][1], b.world.m[3][2]};
        int tip = -1;
        float length_model = 0.0f;
        if (tips_it != ragdoll_tips_.end() && k < tips_it->second.tips.size()) {
            tip = tips_it->second.tips[k];
            length_model = tips_it->second.lengths[k];
        }
        Vec3 dir{};
        if (tip >= 0) {
            dir = transformPoint(world_matrix, ik::nodePosition(pose, tip)) - origin;
        } else {
            // Sin hijo en la lista: su primer hijo del esqueleto o, si es una
            // punta (cabeza, mano), la direccion desde su padre.
            for (std::size_t c = static_cast<std::size_t>(b.node) + 1; c < data.nodes.size(); ++c) {
                if (data.nodes[c].parent == b.node) {
                    dir = transformPoint(world_matrix, ik::nodePosition(pose, static_cast<int>(c))) - origin;
                    if (core::length(dir) > 1e-5f) break;
                }
            }
            if (core::length(dir) < 1e-5f) {
                const int parent = data.nodes[static_cast<std::size_t>(b.node)].parent;
                if (parent >= 0) dir = origin - transformPoint(world_matrix, ik::nodePosition(pose, parent));
            }
        }
        if (core::length(dir) < 1e-5f) dir = transformDirection(world_matrix, Vec3{0.0f, 1.0f, 0.0f});
        const float scale = maxAxisScale(world_matrix);
        const float wanted = length_model > 0.0f ? length_model * scale : core::length(dir);
        b.tip = origin + core::normalize(dir) * std::max(wanted, b.radius * 2.0f);
    }
    rt.pose_dt = delta_seconds > 0.0f ? delta_seconds : rt.pose_dt;
    rt.has_previous = rt.pose_ready;
    rt.pose_ready = true;

    // La pose de la fisica encima (o volviendo a la animacion).
    float weight = 0.0f;
    if (rt.simulating && rt.sim_ready) {
        weight = std::clamp(ragdoll.blend, 0.0f, 1.0f);
    } else if (rt.blend_out > 0.0f && rt.sim_ready) {
        rt.blend_out = std::max(rt.blend_out - delta_seconds, 0.0f);
        weight = rt.blend_out_total > 1e-4f ? rt.blend_out / rt.blend_out_total : 0.0f;
    }
    if (weight <= 0.0f) return;
    const Mat4 to_model = core::inverse(world_matrix);
    for (const RagdollRuntime::Bone& b : rt.bones) {
        Vec3 t{};
        Quat r{};
        Vec3 s{};
        decomposeMatrix(b.world, t, r, s);
        Vec3 tt{};
        Quat tr{};
        Vec3 ts{};
        decomposeMatrix(to_model * core::composeTrs(b.sim_position, b.sim_rotation, s), tt, tr, ts);
        ik::setGlobalRotation(pose, b.node, core::slerp(ik::nodeRotation(pose, b.node), tr, weight));
        if (b.parent < 0) ik::translateGlobal(pose, b.node, (tt - ik::nodePosition(pose, b.node)) * weight);
    }
}

// --- Sockets que siguen a un hueso -------------------------------------------------

Entity RenderSync::skinnedEntity(World& world, Entity e, const std::string* bone) {
    const auto matches = [&](Entity x) {
        const auto it = animations_.find(x.handle());
        if (it == animations_.end() || it->second.seen + 2 < frame_) return false;
        const asset::ModelData* data = it->second.animator.model();
        if (data == nullptr || data->bones.empty()) return false;
        return bone == nullptr || findBone(*data, *bone) >= 0;
    };
    if (!e.valid()) return {};
    std::vector<Entity> stack{e};
    while (!stack.empty()) {
        const Entity x = stack.back();
        stack.pop_back();
        if (matches(x)) return x;
        for (auto it = x.children().rbegin(); it != x.children().rend(); ++it) stack.push_back(world.wrap(*it));
    }
    return {};
}

void RenderSync::updateSockets(World& world, scene::Scene& scene) {
    drive_sockets_.clear();
    std::vector<entt::entity> moved;
    for (const entt::entity h : world.registry().view<BoneSocket>()) {
        Entity s = world.wrap(h);
        if (!s.activeInHierarchy()) continue;
        const BoneSocket& socket = s.get<BoneSocket>();
        if (socket.bone.empty()) continue;
        // El modelo: el guardado si sigue valiendo; si no, se busca subiendo
        // por los antepasados (el modelo suele ser un hermano o un tio).
        Entity source;
        if (const auto it = socket_sources_.find(h); it != socket_sources_.end()) {
            const Entity cached = world.wrap(it->second);
            if (world.valid(it->second) && skinnedEntity(world, cached, &socket.bone) == cached) source = cached;
        }
        if (!source.valid()) {
            for (Entity a = s.parent(); a.valid() && !source.valid(); a = a.parent()) source = skinnedEntity(world, a, &socket.bone);
            if (!source.valid()) {
                socket_sources_.erase(h);
                continue;
            }
            socket_sources_[h] = source.handle();
        }
        if (socket.mode == SocketMode::Drive) {
            drive_sockets_[source.handle()].push_back(h);
            continue;
        }
        AnimationState& state = animations_[source.handle()];
        const asset::ModelData* data = state.animator.model();
        const int node = data != nullptr ? findBone(*data, socket.bone) : -1;
        if (node < 0 || static_cast<std::size_t>(node) >= state.animator.globals().size()) continue;
        const Mat4 bone_world = source.worldMatrix() * state.animator.globals()[static_cast<std::size_t>(node)];
        const Mat4 m = withoutScale(bone_world) *
                       core::composeTrs(socket.position, quatFromEulerDegrees(socket.rotation), s.localScale());
        s.setWorldMatrix(m);
        moved.push_back(h);
    }
    if (moved.empty()) return;
    // Lo que cuelga de un socket movido se dibuja ya en su sitio nuevo.
    std::vector<scene::Actor>& actors = scene.actors();
    for (std::size_t i = 0; i < actor_entities_.size() && i < actors.size(); ++i) {
        Entity x = world.wrap(actor_entities_[i]);
        bool under = false;
        for (Entity a = x; a.valid() && !under; a = a.parent()) {
            under = std::find(moved.begin(), moved.end(), a.handle()) != moved.end();
        }
        if (!under) continue;
        const Mat4& m = x.worldMatrix();
        const Vec3 shift = transformPoint(m, Vec3{}) - transformPoint(actors[i].transform, Vec3{});
        actors[i].transform = m;
        actors[i].bounds_center = actors[i].bounds_center + shift;
    }
}

// --- Consultas (Lua, editor) -------------------------------------------------------

bool RenderSync::boneWorld(World& world, Entity e, const std::string& bone, Mat4& out) {
    const Entity source = skinnedEntity(world, e, &bone);
    if (!source.valid()) return false;
    AnimationState& state = animations_[source.handle()];
    const int node = findBone(*state.animator.model(), bone);
    if (node < 0 || static_cast<std::size_t>(node) >= state.animator.globals().size()) return false;
    out = source.worldMatrix() * state.animator.globals()[static_cast<std::size_t>(node)];
    return true;
}

bool RenderSync::skeletonPose(World& world, Entity e, SkeletonPose& out) {
    out = SkeletonPose{};
    const Entity source = skinnedEntity(world, e, nullptr);
    if (!source.valid()) return false;
    AnimationState& state = animations_[source.handle()];
    const asset::ModelData& data = *state.animator.model();
    if (state.animator.globals().size() != data.nodes.size()) return false;
    // Solo los nodos del esqueleto: los huesos y sus antepasados (sin la raiz
    // de la escena ni las mallas).
    std::vector<bool> keep(data.nodes.size(), false);
    for (const asset::Bone& b : data.bones) {
        for (int n = b.node; n > 0 && !keep[static_cast<std::size_t>(n)]; n = data.nodes[static_cast<std::size_t>(n)].parent) {
            keep[static_cast<std::size_t>(n)] = true;
        }
    }
    std::vector<int> index(data.nodes.size(), -1);
    const Mat4& w = source.worldMatrix();
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
        if (!keep[i]) continue;
        index[i] = static_cast<int>(out.names.size());
        out.names.push_back(data.nodes[i].name);
        out.nodes.push_back(static_cast<int>(i));
        const Mat4 m = w * state.animator.globals()[i];
        out.positions.push_back(Vec3{m.m[3][0], m.m[3][1], m.m[3][2]});
        const int parent = data.nodes[i].parent;
        out.parents.push_back(parent >= 0 ? index[static_cast<std::size_t>(parent)] : -1);
    }
    out.source = source;
    out.scale = maxAxisScale(w);
    return !out.names.empty();
}

const std::vector<physbone::Chain>* RenderSync::physBoneChains(World& world, Entity e) {
    for (Entity x = skinnedEntity(world, e, nullptr); x.valid();) {
        const auto it = physbones_.find(x.handle());
        return it != physbones_.end() ? &it->second.chains : nullptr;
    }
    return nullptr;
}

const asset::ModelData* RenderSync::skeletonData(World& world, Entity e, float* scale) {
    const Entity source = skinnedEntity(world, e, nullptr);
    if (!source.valid()) return nullptr;
    if (scale != nullptr) *scale = maxAxisScale(source.worldMatrix());
    return animations_[source.handle()].animator.model();
}

}  // namespace cramion::ecs
