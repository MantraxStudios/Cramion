#include "CramionCore/anim/Humanoid.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>

namespace cramion::humanoid {

using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

constexpr std::array<const char*, kBoneCount> kNames = {
    "Hips",          "Spine",         "Chest",         "UpperChest",    "Neck",         "Head",
    "LeftShoulder",  "LeftUpperArm",  "LeftLowerArm",  "LeftHand",      "RightShoulder", "RightUpperArm",
    "RightLowerArm", "RightHand",     "LeftUpperLeg",  "LeftLowerLeg",  "LeftFoot",      "LeftToes",
    "RightUpperLeg", "RightLowerLeg", "RightFoot",     "RightToes"};

// --- Nombres ------------------------------------------------------------------

enum class Side { None, Left, Right };

// Parte un nombre en palabras en minusculas: "mixamorig:LeftUpLeg" ->
// [left, up, leg]; "upperarm_l" -> [upperarm, l]; "Bip01 L Calf" -> [l, calf].
// Sin el espacio de nombres (hasta ':' o '|') ni numeros al final.
std::vector<std::string> words(std::string name) {
    if (const auto cut = name.find_last_of(":|"); cut != std::string::npos) name = name.substr(cut + 1);
    std::vector<std::string> out;
    std::string current;
    const auto flush = [&] {
        while (!current.empty() && std::isdigit(static_cast<unsigned char>(current.back()))) current.pop_back();
        if (!current.empty()) out.push_back(current);
        current.clear();
    };
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(c)) {
            flush();
            continue;
        }
        // Mayuscula tras minuscula: palabra nueva (camelCase).
        if (std::isupper(c) && !current.empty() && std::islower(static_cast<unsigned char>(name[i - 1]))) flush();
        current.push_back(static_cast<char>(std::tolower(c)));
    }
    flush();
    // Prefijos de programas: "bip01", "bip001", "def" (Rigify), "b" (algunos juegos).
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const std::string& w) { return w.rfind("bip", 0) == 0 || w == "def" || w == "org"; }),
              out.end());
    return out;
}

// Lado y clave sin lado ("left","l" / "right","r" fuera): [left, up, leg] -> "upleg".
std::string keyOf(const std::string& name, Side& side, bool& helper) {
    side = Side::None;
    helper = false;
    std::string key;
    for (const std::string& w : words(name)) {
        if (w == "left" || w == "l") {
            side = Side::Left;
            continue;
        }
        if (w == "right" || w == "r") {
            side = Side::Right;
            continue;
        }
        // Huesos de ayuda que no son el hueso de verdad.
        if (w == "twist" || w == "roll" || w == "end" || w == "nub" || w == "ik" || w == "ctrl" || w == "target" ||
            w == "pole" || w == "helper" || w == "corrective" || w == "fk" || w == "mch") {
            helper = true;
        }
        key += w;
    }
    return key;
}

struct Pattern {
    Bone left;
    Bone right;  // = left si no tiene lado
    std::vector<const char*> keys;
};

const std::vector<Pattern>& patterns() {
    static const std::vector<Pattern> list = {
        {Bone::Hips, Bone::Hips, {"hips", "hip", "pelvis"}},
        {Bone::Neck, Bone::Neck, {"neck"}},
        {Bone::Head, Bone::Head, {"head"}},
        {Bone::LeftShoulder, Bone::RightShoulder, {"shoulder", "clavicle", "collar", "collarbone"}},
        {Bone::LeftUpperArm, Bone::RightUpperArm, {"upperarm", "arm", "uparm", "humerus"}},
        {Bone::LeftLowerArm, Bone::RightLowerArm, {"forearm", "lowerarm", "lowarm", "elbow"}},
        {Bone::LeftHand, Bone::RightHand, {"hand", "wrist"}},
        {Bone::LeftUpperLeg, Bone::RightUpperLeg, {"upleg", "upperleg", "thigh", "femur"}},
        {Bone::LeftLowerLeg, Bone::RightLowerLeg, {"leg", "lowerleg", "calf", "shin", "knee", "lowleg"}},
        {Bone::LeftFoot, Bone::RightFoot, {"foot", "ankle"}},
        {Bone::LeftToes, Bone::RightToes, {"toebase", "toe", "toes", "ball"}},
    };
    return list;
}

bool isAncestor(const std::vector<asset::Node>& nodes, int ancestor, int node) {
    for (int n = node >= 0 ? nodes[static_cast<std::size_t>(node)].parent : -1; n >= 0;
         n = nodes[static_cast<std::size_t>(n)].parent) {
        if (n == ancestor) return true;
    }
    return false;
}

// --- Matematicas ----------------------------------------------------------------

Quat rotationOf(const Mat4& m) {
    Vec3 t{};
    Quat r{};
    Vec3 s{};
    ecs::decomposeMatrix(m, t, r, s);
    return r;
}
Vec3 positionOf(const Mat4& m) { return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]}; }
Quat inverse(const Quat& q) { return ecs::quatConjugate(q); }
Quat mul(const Quat& a, const Quat& b) { return ecs::quatMultiply(a, b); }

// Giro minimo que lleva la direccion `a` a la `b` (unitarias).
Quat between(const Vec3& a, const Vec3& b) {
    const float d = core::dot(a, b);
    if (d < -0.9999f) {
        // Opuestas: media vuelta alrededor de cualquier perpendicular.
        Vec3 axis = core::cross(Vec3{1.0f, 0.0f, 0.0f}, a);
        if (core::length(axis) < 1e-4f) axis = core::cross(Vec3{0.0f, 1.0f, 0.0f}, a);
        axis = core::normalize(axis);
        return Quat{axis.x, axis.y, axis.z, 0.0f};
    }
    const Vec3 c = core::cross(a, b);
    return core::normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

// Muestreo de pistas (como anim::Animator).
template <typename Key>
std::pair<std::size_t, float> locate(const std::vector<Key>& keys, float time) {
    if (keys.size() == 1 || time <= keys.front().time) return {0, 0.0f};
    if (time >= keys.back().time) return {keys.size() - 1, 0.0f};
    const auto next = std::upper_bound(keys.begin(), keys.end(), time,
                                       [](float t, const Key& key) { return t < key.time; });
    const auto index = static_cast<std::size_t>(std::distance(keys.begin(), next)) - 1;
    const float span = keys[index + 1].time - keys[index].time;
    return {index, span > 0.0f ? (time - keys[index].time) / span : 0.0f};
}
Vec3 sampleVector(const std::vector<asset::VectorKey>& keys, float time) {
    const auto [i, f] = locate(keys, time);
    return f <= 0.0f ? keys[i].value : core::lerp(keys[i].value, keys[i + 1].value, f);
}
Quat sampleRotation(const std::vector<asset::QuatKey>& keys, float time) {
    const auto [i, f] = locate(keys, time);
    return f <= 0.0f ? keys[i].value : core::slerp(keys[i].value, keys[i + 1].value, f);
}

// Giro del "espacio del personaje" en el modelo: columnas derecha, arriba,
// delante sacadas del reposo (la misma receta en los dos esqueletos).
Quat characterFrame(const Map& map, const std::vector<Mat4>& rest) {
    const auto p = [&](Bone b) { return positionOf(rest[static_cast<std::size_t>(map[b])]); };
    const Vec3 top = map[Bone::Head] >= 0 ? p(Bone::Head) : p(Bone::Neck);
    Vec3 up = core::normalize(top - p(Bone::Hips));
    Vec3 right = p(Bone::RightUpperLeg) - p(Bone::LeftUpperLeg);
    right = right + (p(Bone::RightUpperArm) - p(Bone::LeftUpperArm));
    right = core::normalize(right - up * core::dot(right, up));
    const Vec3 forward = core::cross(right, up);
    Mat4 m = Mat4::identity();
    m.m[0][0] = right.x; m.m[0][1] = right.y; m.m[0][2] = right.z;
    m.m[1][0] = up.x; m.m[1][1] = up.y; m.m[1][2] = up.z;
    m.m[2][0] = forward.x; m.m[2][1] = forward.y; m.m[2][2] = forward.z;
    return ecs::quatFromRotationMatrix(m);
}

// El hueso humano "hijo" que marca la direccion de cada uno (para igualar
// las poses de reposo, T contra A).
Bone childOf(const Map& map, Bone b) {
    const auto has = [&](Bone x) { return map[x] >= 0; };
    switch (b) {
        case Bone::Spine: return has(Bone::Chest) ? Bone::Chest : Bone::Neck;
        case Bone::Chest: return has(Bone::UpperChest) ? Bone::UpperChest : Bone::Neck;
        case Bone::UpperChest: return Bone::Neck;
        case Bone::Neck: return Bone::Head;
        case Bone::LeftShoulder: return Bone::LeftUpperArm;
        case Bone::RightShoulder: return Bone::RightUpperArm;
        case Bone::LeftUpperArm: return Bone::LeftLowerArm;
        case Bone::RightUpperArm: return Bone::RightLowerArm;
        case Bone::LeftLowerArm: return Bone::LeftHand;
        case Bone::RightLowerArm: return Bone::RightHand;
        case Bone::LeftUpperLeg: return Bone::LeftLowerLeg;
        case Bone::RightUpperLeg: return Bone::RightLowerLeg;
        case Bone::LeftLowerLeg: return Bone::LeftFoot;
        case Bone::RightLowerLeg: return Bone::RightFoot;
        case Bone::LeftFoot: return Bone::LeftToes;
        case Bone::RightFoot: return Bone::RightToes;
        default: return Bone::Count;
    }
}

}  // namespace

void characterAxes(const Map& map, const std::vector<Mat4>& rest, Vec3& right, Vec3& up, Vec3& forward) {
    const Quat frame = characterFrame(map, rest);
    right = ecs::quatRotate(frame, Vec3{1.0f, 0.0f, 0.0f});
    up = ecs::quatRotate(frame, Vec3{0.0f, 1.0f, 0.0f});
    // characterFrame guarda cross(derecha, arriba), que es hacia atras.
    forward = core::cross(up, right);
}

const char* boneName(Bone bone) {
    const int i = static_cast<int>(bone);
    return i >= 0 && i < kBoneCount ? kNames[static_cast<std::size_t>(i)] : "?";
}

std::vector<Mat4> restGlobals(const std::vector<asset::Node>& nodes) {
    std::vector<Mat4> globals(nodes.size(), Mat4::identity());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const int parent = nodes[i].parent;
        globals[i] = parent >= 0 ? globals[static_cast<std::size_t>(parent)] * nodes[i].local : nodes[i].local;
    }
    return globals;
}

Map detect(const std::vector<asset::Node>& nodes) {
    Map map;
    map.nodes.fill(-1);
    // Candidatos de cada hueso por nombre, en el orden del esqueleto.
    std::array<std::vector<int>, kBoneCount> candidates;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        Side side = Side::None;
        bool helper = false;
        const std::string key = keyOf(nodes[i].name, side, helper);
        if (helper || key.empty()) continue;
        for (const Pattern& p : patterns()) {
            if (std::find_if(p.keys.begin(), p.keys.end(), [&](const char* k) { return key == k; }) == p.keys.end()) {
                continue;
            }
            const bool sided = p.left != p.right;
            if (sided && side == Side::None) break;       // un brazo sin lado: no sirve
            if (!sided && side != Side::None) break;      // una "cabeza izquierda": tampoco
            const Bone bone = side == Side::Right ? p.right : p.left;
            candidates[static_cast<std::size_t>(bone)].push_back(static_cast<int>(i));
            break;
        }
    }
    // Esqueletos que llaman "Leg" al muslo y "Shin"/"Calf" a la espinilla
    // (Motifect, algunos BVH): sin muslo por nombre, de dos "piernas bajas"
    // encadenadas la de arriba es el muslo.
    for (const auto& [upper, lower] : {std::pair{Bone::LeftUpperLeg, Bone::LeftLowerLeg},
                                       std::pair{Bone::RightUpperLeg, Bone::RightLowerLeg}}) {
        auto& ups = candidates[static_cast<std::size_t>(upper)];
        auto& lows = candidates[static_cast<std::size_t>(lower)];
        if (!ups.empty() || lows.size() < 2) continue;
        for (std::size_t a = 0; a < lows.size() && ups.empty(); ++a) {
            for (std::size_t b = 0; b < lows.size(); ++b) {
                if (a != b && isAncestor(nodes, lows[a], lows[b])) {
                    ups.push_back(lows[a]);
                    lows.erase(lows.begin() + static_cast<std::ptrdiff_t>(a));
                    break;
                }
            }
        }
    }
    // Elige el primero que cuelgue de donde debe (la pierna, de la cadera...).
    const auto pick = [&](Bone bone, int ancestor) {
        for (const int c : candidates[static_cast<std::size_t>(bone)]) {
            if (ancestor < 0 || isAncestor(nodes, ancestor, c)) {
                map[bone] = c;
                return;
            }
        }
    };
    pick(Bone::Hips, -1);
    const int hips = map[Bone::Hips];
    if (hips < 0) return map;
    pick(Bone::Head, hips);
    pick(Bone::Neck, hips);
    if (map[Bone::Neck] < 0 && map[Bone::Head] >= 0) map[Bone::Neck] = nodes[static_cast<std::size_t>(map[Bone::Head])].parent;
    for (int side = 0; side < 2; ++side) {
        const bool left = side == 0;
        const auto b = [&](Bone l, Bone r) { return left ? l : r; };
        pick(b(Bone::LeftUpperLeg, Bone::RightUpperLeg), hips);
        pick(b(Bone::LeftLowerLeg, Bone::RightLowerLeg), map[b(Bone::LeftUpperLeg, Bone::RightUpperLeg)]);
        pick(b(Bone::LeftFoot, Bone::RightFoot), map[b(Bone::LeftLowerLeg, Bone::RightLowerLeg)]);
        pick(b(Bone::LeftToes, Bone::RightToes), map[b(Bone::LeftFoot, Bone::RightFoot)]);
        pick(b(Bone::LeftShoulder, Bone::RightShoulder), hips);
        const int shoulder = map[b(Bone::LeftShoulder, Bone::RightShoulder)];
        pick(b(Bone::LeftUpperArm, Bone::RightUpperArm), shoulder >= 0 ? shoulder : hips);
        pick(b(Bone::LeftLowerArm, Bone::RightLowerArm), map[b(Bone::LeftUpperArm, Bone::RightUpperArm)]);
        pick(b(Bone::LeftHand, Bone::RightHand), map[b(Bone::LeftLowerArm, Bone::RightLowerArm)]);
    }
    // Columna por la estructura: los nodos entre la cadera y el cuello.
    if (map[Bone::Neck] >= 0) {
        std::vector<int> path;
        for (int n = nodes[static_cast<std::size_t>(map[Bone::Neck])].parent; n >= 0 && n != hips;
             n = nodes[static_cast<std::size_t>(n)].parent) {
            path.push_back(n);
        }
        std::reverse(path.begin(), path.end());  // de la cadera hacia arriba
        if (!path.empty()) map[Bone::Spine] = path.front();
        if (path.size() >= 3) {
            map[Bone::Chest] = path[path.size() / 2];
            map[Bone::UpperChest] = path.back();
            if (map[Bone::Chest] == map[Bone::UpperChest]) map[Bone::UpperChest] = -1;
        } else if (path.size() == 2) {
            map[Bone::Chest] = path.back();
        }
    }
    static constexpr Bone kRequired[] = {Bone::Hips,          Bone::Spine,         Bone::Head,         Bone::LeftUpperArm,
                                         Bone::LeftLowerArm,  Bone::LeftHand,      Bone::RightUpperArm, Bone::RightLowerArm,
                                         Bone::RightHand,     Bone::LeftUpperLeg,  Bone::LeftLowerLeg,  Bone::LeftFoot,
                                         Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot};
    map.valid = std::all_of(std::begin(kRequired), std::end(kRequired), [&](Bone b) { return map[b] >= 0; });
    return map;
}

bool retargetClip(const std::vector<asset::Node>& source, const asset::AnimationClip& clip,
                  const std::vector<asset::Node>& target, asset::AnimationClip& out, std::string* error, float fps) {
    const Map ms = detect(source);
    const Map mt = detect(target);
    if (!ms.valid || !mt.valid) {
        if (error) *error = !ms.valid ? "el esqueleto del clip no es humanoide" : "el modelo no es humanoide";
        return false;
    }
    const std::vector<Mat4> rest_s = restGlobals(source);
    const std::vector<Mat4> rest_t = restGlobals(target);
    const Quat cs = characterFrame(ms, rest_s);
    const Quat ct = characterFrame(mt, rest_t);
    const Quat cs_inv = inverse(cs);
    const Quat ct_inv = inverse(ct);

    // Por hueso humano: giro de reposo de cada lado y el ajuste que pone el
    // hueso del destino en la direccion de reposo del de origen (T contra A).
    std::array<Quat, kBoneCount> rest_rot_s{};
    std::array<Quat, kBoneCount> rest_rot_t{};
    std::array<Quat, kBoneCount> align{};
    for (int b = 0; b < kBoneCount; ++b) {
        const Bone bone = static_cast<Bone>(b);
        if (ms[bone] < 0 || mt[bone] < 0) continue;
        rest_rot_s[static_cast<std::size_t>(b)] = rotationOf(rest_s[static_cast<std::size_t>(ms[bone])]);
        rest_rot_t[static_cast<std::size_t>(b)] = rotationOf(rest_t[static_cast<std::size_t>(mt[bone])]);
        align[static_cast<std::size_t>(b)] = Quat{};
        const Bone child = childOf(mt, bone);
        if (child != Bone::Count && ms[child] >= 0 && mt[child] >= 0) {
            const Vec3 ds = positionOf(rest_s[static_cast<std::size_t>(ms[child])]) -
                            positionOf(rest_s[static_cast<std::size_t>(ms[bone])]);
            const Vec3 dt = positionOf(rest_t[static_cast<std::size_t>(mt[child])]) -
                            positionOf(rest_t[static_cast<std::size_t>(mt[bone])]);
            if (core::length(ds) > 1e-6f && core::length(dt) > 1e-6f) {
                // La direccion de reposo del origen, llevada al modelo destino.
                const Vec3 ds_in_t = ecs::quatRotate(ct, ecs::quatRotate(cs_inv, core::normalize(ds)));
                align[static_cast<std::size_t>(b)] = between(core::normalize(dt), ds_in_t);
            }
        }
    }
    // Escala de la cadera: altura de la cadera sobre los pies de cada uno.
    const auto hips_height = [](const Map& m, const std::vector<Mat4>& rest, const Quat& frame) {
        const Vec3 hips = positionOf(rest[static_cast<std::size_t>(m[Bone::Hips])]);
        const Vec3 feet = (positionOf(rest[static_cast<std::size_t>(m[Bone::LeftFoot])]) +
                           positionOf(rest[static_cast<std::size_t>(m[Bone::RightFoot])])) *
                          0.5f;
        return std::max(ecs::quatRotate(ecs::quatConjugate(frame), hips - feet).y, 1e-4f);
    };
    const float hip_scale = hips_height(mt, rest_t, ct) / hips_height(ms, rest_s, cs);

    // Canal del clip por nodo del origen.
    std::vector<const asset::AnimationChannel*> channel(source.size(), nullptr);
    for (const asset::AnimationChannel& c : clip.channels) {
        if (c.node >= 0 && static_cast<std::size_t>(c.node) < source.size()) channel[static_cast<std::size_t>(c.node)] = &c;
    }
    // Nodo del destino -> hueso humano.
    std::vector<int> target_bone(target.size(), -1);
    for (int b = 0; b < kBoneCount; ++b) {
        if (mt[static_cast<Bone>(b)] >= 0 && ms[static_cast<Bone>(b)] >= 0) {
            target_bone[static_cast<std::size_t>(mt[static_cast<Bone>(b)])] = b;
        }
    }

    out = asset::AnimationClip{};
    out.name = clip.name;
    out.duration = clip.duration;
    std::vector<asset::AnimationChannel> channels(target.size());
    for (std::size_t i = 0; i < target.size(); ++i) channels[i].node = static_cast<std::int32_t>(i);

    const int frames = std::max(2, static_cast<int>(std::ceil(clip.duration * fps)) + 1);
    std::vector<Mat4> global_s(source.size());
    std::vector<Mat4> global_t(target.size());
    const int hips_t = mt[Bone::Hips];
    Vec3 hips_rest_s = positionOf(rest_s[static_cast<std::size_t>(ms[Bone::Hips])]);
    {
        // Reposo que no pisa el suelo (BVH/Motifect: la cadera en el origen y
        // los pies un metro por debajo) mientras el clip si lo pisa: el
        // desplazamiento de la cadera se mide desde ese reposo puesto de pie.
        float ground = 0.0f;
        bool any = false;
        for (const Bone b : {Bone::LeftFoot, Bone::RightFoot, Bone::LeftToes, Bone::RightToes}) {
            if (ms[b] < 0) continue;
            const float y = ecs::quatRotate(cs_inv, positionOf(rest_s[static_cast<std::size_t>(ms[b])])).y;
            ground = any ? std::min(ground, y) : y;
            any = true;
        }
        const float hips_up = ecs::quatRotate(cs_inv, hips_rest_s).y;
        if (any && ground < -0.25f * std::max(hips_up - ground, 1e-4f)) {
            hips_rest_s = hips_rest_s - ecs::quatRotate(cs, Vec3{0.0f, ground, 0.0f});
        }
    }
    const Vec3 hips_rest_t = positionOf(rest_t[static_cast<std::size_t>(hips_t)]);
    for (int f = 0; f < frames; ++f) {
        const float time = std::min(static_cast<float>(f) / fps, clip.duration);
        // --- Pose del origen (FK) ---
        for (std::size_t i = 0; i < source.size(); ++i) {
            Mat4 local = source[i].local;
            if (const asset::AnimationChannel* c = channel[i]) {
                Vec3 t{};
                Quat r{};
                Vec3 s{};
                ecs::decomposeMatrix(local, t, r, s);
                if (!c->positions.empty()) t = sampleVector(c->positions, time);
                if (!c->rotations.empty()) r = sampleRotation(c->rotations, time);
                if (!c->scales.empty()) s = sampleVector(c->scales, time);
                local = core::composeTrs(t, r, s);
            }
            const int parent = source[i].parent;
            global_s[i] = parent >= 0 ? global_s[static_cast<std::size_t>(parent)] * local : local;
        }
        // --- Pose del destino, de padres a hijos ---
        for (std::size_t i = 0; i < target.size(); ++i) {
            const int parent = target[i].parent;
            const Mat4 parent_global = parent >= 0 ? global_t[static_cast<std::size_t>(parent)] : Mat4::identity();
            const int b = target_bone[i];
            if (b < 0) {
                global_t[i] = parent_global * target[i].local;
                continue;
            }
            // Giro del hueso de origen respecto a su reposo, en el personaje.
            const Quat anim_s = rotationOf(global_s[static_cast<std::size_t>(ms[static_cast<Bone>(b)])]);
            const Quat delta_s = mul(anim_s, inverse(rest_rot_s[static_cast<std::size_t>(b)]));
            const Quat delta_t = mul(mul(ct, mul(cs_inv, mul(delta_s, cs))), ct_inv);
            const Quat rotation =
                core::normalize(mul(delta_t, mul(align[static_cast<std::size_t>(b)], rest_rot_t[static_cast<std::size_t>(b)])));
            // Posicion: la de reposo colgando del padre animado (mantiene las
            // medidas del destino); la cadera, desplazada como la de origen.
            Vec3 position = positionOf(parent_global * target[i].local);
            if (static_cast<int>(i) == hips_t) {
                const Vec3 moved = positionOf(global_s[static_cast<std::size_t>(ms[Bone::Hips])]) - hips_rest_s;
                position = hips_rest_t + ecs::quatRotate(ct, ecs::quatRotate(cs_inv, moved)) * hip_scale;
            }
            Vec3 rest_t_pos{};
            Quat rest_t_rot{};
            Vec3 scale{};
            ecs::decomposeMatrix(rest_t[i], rest_t_pos, rest_t_rot, scale);
            global_t[i] = core::composeTrs(position, rotation, scale);
            // Local para el clip.
            const Mat4 local = core::inverse(parent_global) * global_t[i];
            Vec3 lt{};
            Quat lr{};
            Vec3 ls{};
            ecs::decomposeMatrix(local, lt, lr, ls);
            asset::AnimationChannel& c = channels[i];
            // Mismo hemisferio que la clave anterior (q y -q son el mismo
            // giro, pero el slerp daria la vuelta larga).
            if (!c.rotations.empty()) {
                const Quat& prev = c.rotations.back().value;
                if (prev.x * lr.x + prev.y * lr.y + prev.z * lr.z + prev.w * lr.w < 0.0f) {
                    lr = Quat{-lr.x, -lr.y, -lr.z, -lr.w};
                }
            }
            c.rotations.push_back({time, lr});
            if (static_cast<int>(i) == hips_t) c.positions.push_back({time, lt});
        }
    }
    for (asset::AnimationChannel& c : channels) {
        if (!c.rotations.empty() || !c.positions.empty()) out.channels.push_back(std::move(c));
    }
    return true;
}

}  // namespace cramion::humanoid
