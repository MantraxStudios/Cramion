#include "CramionCore/ecs/Rigging.h"

#include "CramionCore/anim/Creature.h"
#include "CramionCore/anim/Humanoid.h"
#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>

namespace cramion::ecs {

namespace {

// La palabra empieza una palabra del nombre: al principio, tras un
// separador o en mayuscula ("Ear_L", "LeftEar", "ear.L"; no "ForeArm").
bool hasWord(const std::string& original, std::initializer_list<const char*> words) {
    std::string name = original;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* w : words) {
        for (std::size_t at = name.find(w); at != std::string::npos; at = name.find(w, at + 1)) {
            const unsigned char before = at > 0 ? static_cast<unsigned char>(original[at - 1]) : ' ';
            const unsigned char first = static_cast<unsigned char>(original[at]);
            if (at == 0 || !std::isalpha(before) || (std::isupper(first) && std::islower(before))) return true;
        }
    }
    return false;
}

}  // namespace

int findBone(const asset::ModelData& data, const std::string& name) {
    if (name.empty()) return -1;
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
        if (data.nodes[i].name == name) return static_cast<int>(i);
    }
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
        const std::string& n = data.nodes[i].name;
        const auto cut = n.find_last_of(":|");
        if (cut != std::string::npos && n.compare(cut + 1, std::string::npos, name) == 0) return static_cast<int>(i);
    }
    // Sin distinguir mayusculas.
    const std::string wanted = creature::cleanName(name);
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
        if (creature::cleanName(data.nodes[i].name) == wanted) return static_cast<int>(i);
    }
    return -1;
}

std::vector<RagdollBoneSetting> suggestRagdollBones(const asset::ModelData& data, float scale) {
    std::vector<RagdollBoneSetting> out;
    const std::vector<core::Mat4> rest = humanoid::restGlobals(data.nodes);
    const std::vector<creature::RagdollBone> bones = creature::ragdollBones(data.nodes, rest);
    for (const creature::RagdollBone& b : bones) {
        RagdollBoneSetting s;
        s.bone = data.nodes[static_cast<std::size_t>(b.node)].name;
        s.radius = b.radius * scale;
        s.length = b.length * scale;
        s.mass = 0.0f;  // reparto automatico de la masa total
        s.swing = b.swing;
        s.twist = b.twist;
        out.push_back(s);
    }
    return out;
}

bool suggestCreatureIK(const asset::ModelData& data, InverseKinematics& ik) {
    const std::vector<core::Mat4> rest = humanoid::restGlobals(data.nodes);
    const creature::Rig rig = creature::detect(data.nodes, rest);
    const bool human = humanoid::detect(data.nodes).valid;
    if (!rig.valid && !human) return false;
    // Las cadenas al suelo de antes se rehacen; las demas se quedan.
    ik.chains.erase(std::remove_if(ik.chains.begin(), ik.chains.end(), [](const IKChain& c) { return c.ground; }),
                    ik.chains.end());
    if (human) {
        ik.foot_grounding = true;
    } else {
        for (const creature::Leg& leg : rig.legs) {
            IKChain c;
            c.bone = data.nodes[static_cast<std::size_t>(leg.foot)].name;
            c.length = leg.bones;
            c.ground = true;
            ik.chains.push_back(c);
        }
        if (rig.head >= 0) {
            ik.look_bone = data.nodes[static_cast<std::size_t>(rig.head)].name;
            ik.look_chain = std::clamp(static_cast<int>(rig.neck.size()) + 1, 1, 4);
        }
        ik.align_body = rig.legs.size() >= 4;
    }
    return true;
}

std::vector<PhysBoneChain> suggestPhysBones(const asset::ModelData& data) {
    std::vector<PhysBoneChain> out;
    const std::size_t count = data.nodes.size();
    std::vector<int> child_count(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        if (data.nodes[i].parent >= 0) ++child_count[static_cast<std::size_t>(data.nodes[i].parent)];
    }
    enum class Kind { None, Hair, Tail, Ear, Cloth, Thin };
    const auto kind_of = [&](std::size_t i) {
        // El nombre sin prefijo pero con sus mayusculas (para separar palabras).
        std::string n = data.nodes[i].name;
        if (const auto cut = n.find_last_of(":|"); cut != std::string::npos) n = n.substr(cut + 1);
        if (hasWord(n, {"hair", "pelo", "ponytail", "coleta", "bang", "flequillo", "braid", "trenza"})) return Kind::Hair;
        if (hasWord(n, {"tail", "cola"})) return Kind::Tail;
        if (hasWord(n, {"ear", "oreja"}) && !hasWord(n, {"earring", "pendiente"})) return Kind::Ear;
        if (hasWord(n, {"skirt", "falda", "cape", "capa", "cloak", "coat", "dress", "vestido", "cloth", "tela"})) return Kind::Cloth;
        if (hasWord(n, {"ribbon", "cinta", "antenna", "antena", "chain", "cadena", "earring", "pendiente", "tie", "corbata",
                        "scarf", "bufanda", "strap", "whisker", "bigote", "tentacle", "tentaculo"})) {
            return Kind::Thin;
        }
        return Kind::None;
    };
    for (std::size_t i = 0; i < count; ++i) {
        const Kind k = kind_of(i);
        if (k == Kind::None || child_count[i] == 0) continue;
        // Solo la raiz de cada cadena (su padre no es del mismo tipo).
        const int parent = data.nodes[i].parent;
        if (parent >= 0 && kind_of(static_cast<std::size_t>(parent)) != Kind::None) continue;
        PhysBoneChain c;
        c.bone = data.nodes[i].name;
        c.end_length = 0.5f;
        switch (k) {
            case Kind::Hair:
                c.pull = 0.2f; c.spring = 0.2f; c.stiffness = 0.2f; c.gravity = 0.3f; c.gravity_falloff = 0.6f;
                c.radius = 0.02f;
                break;
            case Kind::Tail:
                c.pull = 0.15f; c.spring = 0.35f; c.stiffness = 0.1f; c.gravity = 0.15f; c.gravity_falloff = 0.5f;
                c.radius = 0.03f; c.radius_tip = 0.015f;
                break;
            case Kind::Ear:
                c.pull = 0.5f; c.spring = 0.3f; c.stiffness = 0.5f; c.max_angle = 45.0f; c.radius = 0.015f;
                break;
            case Kind::Cloth:
                c.pull = 0.1f; c.spring = 0.1f; c.stiffness = 0.05f; c.gravity = 0.5f; c.immobile = 0.3f;
                c.max_angle = 70.0f; c.radius = 0.03f;
                break;
            case Kind::Thin:
            case Kind::None:
                c.pull = 0.1f; c.spring = 0.4f; c.stiffness = 0.05f; c.gravity = 0.4f; c.radius = 0.01f;
                break;
        }
        out.push_back(c);
    }
    return out;
}

// --- Huesos como objetos (sin RenderSync) ------------------------------------------------

std::vector<BindBone> bindSkeleton(const asset::ModelData& data) {
    std::vector<BindBone> out;
    const std::size_t count = data.nodes.size();
    // Los huesos y sus antepasados, sin el nodo 0 (la raiz de la escena del
    // archivo), como RenderSync::skeletonPose.
    std::vector<bool> keep(count, false);
    for (const asset::Bone& b : data.bones) {
        for (int n = b.node; n > 0 && static_cast<std::size_t>(n) < count && !keep[static_cast<std::size_t>(n)];
             n = data.nodes[static_cast<std::size_t>(n)].parent) {
            keep[static_cast<std::size_t>(n)] = true;
        }
    }
    const std::vector<core::Mat4> rest = humanoid::restGlobals(data.nodes);
    std::vector<int> index(count, -1);
    // Los nodos vienen con los padres antes que los hijos.
    for (std::size_t i = 0; i < count; ++i) {
        if (!keep[i]) continue;
        BindBone bone;
        bone.name = data.nodes[i].name;
        bone.node = static_cast<int>(i);
        const int parent = data.nodes[i].parent;
        bone.parent = parent >= 0 && static_cast<std::size_t>(parent) < i ? index[static_cast<std::size_t>(parent)] : -1;
        bone.global = rest[i];
        index[i] = static_cast<int>(out.size());
        out.push_back(std::move(bone));
    }
    return out;
}

Entity findBoneObject(Entity skinned, const std::string& bone) {
    if (!skinned.valid() || bone.empty() || skinned.world() == nullptr) return {};
    World& world = *skinned.world();
    std::vector<Entity> stack{skinned};
    while (!stack.empty()) {
        const Entity e = stack.back();
        stack.pop_back();
        // Los de antes ("Huesos como objetos" del editor) no tienen la marca
        // pero se llaman como su hueso.
        if (const BoneSocket* s = e.tryGet<BoneSocket>();
            s != nullptr && s->bone == bone && (s->bone_object || s->bone == e.name())) {
            return e;
        }
        const auto& children = e.children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(world.wrap(*it));
    }
    return {};
}

std::vector<Entity> createBoneEntities(World& world, Entity skinned, const asset::ModelData& data,
                                       const std::vector<std::string>& only, bool nested) {
    std::vector<Entity> created;
    if (!skinned.valid()) return created;
    const std::vector<BindBone> skeleton = bindSkeleton(data);
    if (skeleton.empty()) return created;
    const std::unordered_set<std::string> wanted(only.begin(), only.end());
    // Los que ya tienen objeto en este modelo (no se repiten).
    std::unordered_map<std::string, Entity> existing;
    {
        std::vector<Entity> stack{skinned};
        while (!stack.empty()) {
            const Entity e = stack.back();
            stack.pop_back();
            if (const BoneSocket* s = e.tryGet<BoneSocket>();
                s != nullptr && !s->bone.empty() && (s->bone_object || s->bone == e.name())) {
                existing.emplace(s->bone, e);
            }
            for (const entt::entity child : e.children()) stack.push_back(world.wrap(child));
        }
    }
    const core::Mat4 model_world = skinned.worldMatrix();
    // En el orden del esqueleto (los padres antes): con `nested` cada hueso
    // cuelga del objeto de su hueso padre mas cercano, asi mover la mano
    // mueve tambien los dedos.
    std::vector<Entity> objects(skeleton.size());
    for (std::size_t i = 0; i < skeleton.size(); ++i) {
        const BindBone& bone = skeleton[i];
        if (const auto it = existing.find(bone.name); it != existing.end()) {
            objects[i] = it->second;
            continue;
        }
        if (!wanted.empty() && wanted.count(bone.name) == 0) continue;
        Entity parent;
        if (nested) {
            for (int p = bone.parent; p >= 0 && !parent.valid(); p = skeleton[static_cast<std::size_t>(p)].parent) {
                parent = objects[static_cast<std::size_t>(p)];
            }
        }
        if (!parent.valid()) parent = skinned;
        Entity e = world.create(bone.name.empty() ? std::string("Hueso") : bone.name, parent);
        // En el hueso en reposo, sin su escala (el socket no la usa).
        core::Vec3 position{};
        core::Quat rotation{};
        core::Vec3 scale{};
        decomposeMatrix(model_world * bone.global, position, rotation, scale);
        e.setWorldMatrix(core::composeTrs(position, rotation, core::Vec3{1.0f, 1.0f, 1.0f}));
        // Siguen la animacion; el que se mueve con el gizmo pasa a mover su hueso.
        BoneSocket& socket = e.add<BoneSocket>();
        socket.bone = bone.name;
        socket.mode = SocketMode::Follow;
        socket.drive_position = true;
        socket.bone_object = true;
        objects[i] = e;
        created.push_back(e);
    }
    return created;
}

Entity materializeBone(World& world, Entity skinned, const asset::ModelData& data, const std::string& bone,
                       bool nested) {
    if (!skinned.valid()) return {};
    if (Entity found = findBoneObject(skinned, bone); found.valid()) return found;
    // El nombre exacto del nodo ("Head" vale por "mixamorig:Head").
    const int node = findBone(data, bone);
    if (node < 0) return {};
    const std::vector<BindBone> skeleton = bindSkeleton(data);
    int at = -1;
    for (std::size_t i = 0; i < skeleton.size(); ++i) {
        if (skeleton[i].node == node) {
            at = static_cast<int>(i);
            break;
        }
    }
    if (at < 0) return {};
    const std::string& name = skeleton[static_cast<std::size_t>(at)].name;
    if (Entity found = findBoneObject(skinned, name); found.valid()) return found;
    std::vector<std::string> chain{name};
    if (nested) {
        for (int p = skeleton[static_cast<std::size_t>(at)].parent; p >= 0; p = skeleton[static_cast<std::size_t>(p)].parent) {
            chain.push_back(skeleton[static_cast<std::size_t>(p)].name);
        }
    }
    createBoneEntities(world, skinned, data, chain, nested);
    return findBoneObject(skinned, name);
}

}  // namespace cramion::ecs
