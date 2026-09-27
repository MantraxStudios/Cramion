#include "CramionCore/anim/Creature.h"

#include "CramionCore/anim/Humanoid.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <numeric>

namespace cramion::creature {

using core::Mat4;
using core::Vec3;

namespace {

Vec3 position(const Mat4& m) { return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]}; }

bool hasWord(const std::string& name, std::initializer_list<const char*> words) {
    for (const char* w : words) {
        if (name.find(w) != std::string::npos) return true;
    }
    return false;
}

bool isTail(const std::string& n) { return hasWord(n, {"tail", "cola"}); }
bool isHead(const std::string& n) {
    return hasWord(n, {"head", "cabeza", "skull", "craneo"}) && !hasWord(n, {"end", "top", "nub", "tip"});
}
bool isNeck(const std::string& n) { return hasWord(n, {"neck", "cuello"}); }
bool isHelper(const std::string& n) {
    return hasWord(n, {"twist", "helper", "_end", "end_", "nub", "roll", "ik", "target", "pole", "weapon", "prop"});
}

std::vector<int> pathToRoot(const std::vector<asset::Node>& nodes, int n) {
    std::vector<int> path;
    for (; n >= 0; n = nodes[static_cast<std::size_t>(n)].parent) path.push_back(n);
    return path;
}

int ancestorOf(const std::vector<asset::Node>& nodes, const std::vector<int>& of) {
    if (of.empty()) return -1;
    std::vector<int> common = pathToRoot(nodes, of[0]);
    for (std::size_t k = 1; k < of.size(); ++k) {
        const std::vector<int> other = pathToRoot(nodes, of[k]);
        common.erase(std::remove_if(common.begin(), common.end(),
                                    [&](int n) { return std::find(other.begin(), other.end(), n) == other.end(); }),
                     common.end());
    }
    return common.empty() ? -1 : common.front();
}

// Hueso = nodo que tiene hijos o padre dentro del esqueleto (no el nodo de
// la malla ni la raiz vacia). Sin lista de huesos deformantes se usan todos
// los nodos que no son la raiz.
std::vector<bool> boneMask(const std::vector<asset::Node>& nodes) {
    std::vector<bool> mask(nodes.size(), true);
    if (!mask.empty()) mask[0] = false;
    return mask;
}

}  // namespace

std::string cleanName(const std::string& name) {
    std::string n = name;
    const auto cut = n.find_last_of(":|");
    if (cut != std::string::npos) n = n.substr(cut + 1);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return n;
}

float boneLength(const std::vector<asset::Node>& nodes, const std::vector<Mat4>& rest, int node, float fallback) {
    if (node < 0) return fallback;
    const Vec3 p = position(rest[static_cast<std::size_t>(node)]);
    float best = 0.0f;
    for (std::size_t i = static_cast<std::size_t>(node) + 1; i < nodes.size(); ++i) {
        if (nodes[i].parent != node || isHelper(cleanName(nodes[i].name))) continue;
        best = std::max(best, core::length(position(rest[i]) - p));
    }
    return best > 1e-6f ? best : fallback;
}

Rig detect(const std::vector<asset::Node>& nodes, const std::vector<Mat4>& rest) {
    Rig rig;
    const std::size_t count = nodes.size();
    if (count < 3 || rest.size() != count) return rig;
    const std::vector<bool> bone = boneMask(nodes);
    std::vector<std::vector<int>> children(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (nodes[i].parent >= 0) children[static_cast<std::size_t>(nodes[i].parent)].push_back(static_cast<int>(i));
    }
    std::vector<std::string> names(count);
    std::vector<Vec3> p(count);
    float min_y = 1e30f;
    float max_y = -1e30f;
    for (std::size_t i = 0; i < count; ++i) {
        names[i] = cleanName(nodes[i].name);
        p[i] = position(rest[i]);
        if (!bone[i]) continue;
        min_y = std::min(min_y, p[i].y);
        max_y = std::max(max_y, p[i].y);
    }
    rig.height = std::max(max_y - min_y, 1e-4f);
    const float low = min_y + rig.height * 0.2f;         // puntas cerca del suelo
    const float foot_level = min_y + rig.height * 0.12f; // el pie: el primero que casi lo toca
    const auto boneChildren = [&](int n) {
        int c = 0;
        for (const int k : children[static_cast<std::size_t>(n)]) c += bone[static_cast<std::size_t>(k)] && !isHelper(names[static_cast<std::size_t>(k)]) ? 1 : 0;
        return c;
    };
    const auto inTail = [&](int n) {
        for (int a = n; a >= 0; a = nodes[static_cast<std::size_t>(a)].parent) {
            if (isTail(names[static_cast<std::size_t>(a)])) return true;
        }
        return false;
    };

    // --- Patas: de cada punta baja, el primer hueso de su rama cerca del suelo ---
    for (std::size_t leaf = 0; leaf < count; ++leaf) {
        if (!bone[leaf] || boneChildren(static_cast<int>(leaf)) != 0 || p[leaf].y > low) continue;
        if (inTail(static_cast<int>(leaf))) continue;
        std::vector<int> path = pathToRoot(nodes, static_cast<int>(leaf));  // de la punta a la raiz
        // Donde empieza la rama: el primer antepasado cuyo padre se divide.
        int root_index = -1;
        for (std::size_t k = 0; k + 1 < path.size(); ++k) {
            if (boneChildren(path[k + 1]) >= 2) {
                root_index = static_cast<int>(k);
                break;
            }
        }
        if (root_index <= 0) continue;
        // El pie: bajando desde la raiz de la rama, el primero cerca del suelo.
        int foot_index = -1;
        for (int k = root_index; k >= 0; --k) {
            if (p[static_cast<std::size_t>(path[static_cast<std::size_t>(k)])].y <= foot_level) {
                foot_index = k;
                break;
            }
        }
        if (foot_index < 0) continue;
        const int bones_above = root_index - foot_index;  // huesos entre la raiz de la rama y el pie
        if (bones_above < 2) continue;
        const int foot = path[static_cast<std::size_t>(foot_index)];
        // La pata tiene que bajar de verdad (no un hueso colgando del cuerpo).
        const float drop = p[static_cast<std::size_t>(path[static_cast<std::size_t>(root_index)])].y - p[static_cast<std::size_t>(foot)].y;
        if (drop < rig.height * 0.15f) continue;
        if (std::any_of(rig.legs.begin(), rig.legs.end(), [&](const Leg& l) { return l.foot == foot; })) continue;
        Leg leg;
        leg.foot = foot;
        leg.bones = std::min(bones_above, 3);
        rig.legs.push_back(leg);
    }

    // --- Cabeza ---
    int head_depth = 1 << 30;
    for (std::size_t i = 0; i < count; ++i) {
        if (!bone[i] || !isHead(names[i])) continue;
        const int depth = static_cast<int>(pathToRoot(nodes, static_cast<int>(i)).size());
        if (depth < head_depth) {
            head_depth = depth;
            rig.head = static_cast<int>(i);
        }
    }
    if (rig.head < 0) {
        float best = -1e30f;
        for (std::size_t i = 0; i < count; ++i) {
            if (!bone[i] || boneChildren(static_cast<int>(i)) != 0 || inTail(static_cast<int>(i))) continue;
            if (std::any_of(rig.legs.begin(), rig.legs.end(), [&](const Leg& l) {
                    const std::vector<int> path = pathToRoot(nodes, static_cast<int>(i));
                    return std::find(path.begin(), path.end(), l.foot) != path.end();
                })) {
                continue;
            }
            if (p[i].y > best) {
                best = p[i].y;
                rig.head = static_cast<int>(i);
            }
        }
        // La punta suele ser una oreja o el final: la cabeza es su padre si
        // de el salen varias cosas (mandibula, orejas, ojos).
        if (rig.head >= 0) {
            const int parent = nodes[static_cast<std::size_t>(rig.head)].parent;
            if (parent >= 0 && boneChildren(parent) >= 2) rig.head = parent;
        }
    }

    // --- Cuerpo: donde se unen las patas ---
    std::vector<int> leg_roots;
    for (const Leg& l : rig.legs) {
        int top = l.foot;
        for (int k = 0; k < l.bones && top >= 0; ++k) top = nodes[static_cast<std::size_t>(top)].parent;
        if (top >= 0) leg_roots.push_back(top);
    }
    if (leg_roots.size() >= 2) {
        rig.body = ancestorOf(nodes, leg_roots);
    }
    if (rig.body < 0) {
        for (std::size_t i = 1; i < count; ++i) {
            if (bone[i]) {
                rig.body = static_cast<int>(i);
                break;
            }
        }
    }

    // --- Ejes: delante, del cuerpo a la cabeza ---
    if (rig.head >= 0 && rig.body >= 0) {
        Vec3 f = p[static_cast<std::size_t>(rig.head)] - p[static_cast<std::size_t>(rig.body)];
        f.y = 0.0f;
        // Un humano: la cabeza esta encima de la cadera. Delante = hacia donde
        // apuntan los pies (o +Z).
        if (core::length(f) < rig.height * 0.1f) {
            f = Vec3{0.0f, 0.0f, 1.0f};
            const humanoid::Map map = humanoid::detect(nodes);
            if (map.valid) {
                Vec3 right{};
                Vec3 up{};
                humanoid::characterAxes(map, rest, right, up, f);
                f.y = 0.0f;
            }
        }
        if (core::length(f) > 1e-6f) rig.forward = core::normalize(f);
    }
    rig.right = core::normalize(core::cross(rig.forward, rig.up));

    // --- Lados y delante/detras de cada pata ---
    if (!rig.legs.empty()) {
        float mean = 0.0f;
        for (const Leg& l : rig.legs) mean += core::dot(p[static_cast<std::size_t>(l.foot)], rig.forward);
        mean /= static_cast<float>(rig.legs.size());
        const Vec3 center = rig.body >= 0 ? p[static_cast<std::size_t>(rig.body)] : Vec3{};
        for (Leg& l : rig.legs) {
            const Vec3 fp = p[static_cast<std::size_t>(l.foot)];
            l.front = rig.legs.size() > 2 && core::dot(fp, rig.forward) > mean;
            l.left = core::dot(fp - center, rig.right) < 0.0f;
        }
    }

    // --- Columna y cuello: del cuerpo a la cabeza ---
    if (rig.head >= 0 && rig.body >= 0) {
        std::vector<int> path = pathToRoot(nodes, rig.head);
        const auto at = std::find(path.begin(), path.end(), rig.body);
        if (at != path.end()) {
            std::vector<int> between(path.begin() + 1, at);  // de la cabeza hacia el cuerpo, sin ambos
            std::reverse(between.begin(), between.end());   // del cuerpo a la cabeza
            // El cuello: los que tienen nombre de cuello o, si no, los que
            // hay despues de donde nacen las patas delanteras (o el ultimo).
            std::vector<int> front_roots;
            for (std::size_t k = 0; k < rig.legs.size(); ++k) {
                if (rig.legs[k].front && k < leg_roots.size()) front_roots.push_back(leg_roots[k]);
            }
            const int chest = front_roots.size() >= 2 ? ancestorOf(nodes, front_roots) : -1;
            bool after_chest = chest < 0;
            for (const int n : between) {
                if (isNeck(names[static_cast<std::size_t>(n)])) {
                    rig.neck.push_back(n);
                } else if (after_chest && chest >= 0) {
                    rig.neck.push_back(n);
                } else {
                    rig.spine.push_back(n);
                }
                if (n == chest) after_chest = true;
            }
            if (rig.neck.empty() && !rig.spine.empty() && chest < 0 && rig.legs.size() > 2) {
                rig.neck.push_back(rig.spine.back());
                rig.spine.pop_back();
            }
            if (rig.neck.size() > 4) rig.neck.erase(rig.neck.begin(), rig.neck.end() - 4);
        }
    }

    // --- Cola: la base con nombre de cola y su primera rama hasta la punta ---
    int tail_depth = 1 << 30;
    int tail_root = -1;
    for (std::size_t i = 0; i < count; ++i) {
        if (!bone[i] || !isTail(names[i])) continue;
        const int depth = static_cast<int>(pathToRoot(nodes, static_cast<int>(i)).size());
        if (depth < tail_depth) {
            tail_depth = depth;
            tail_root = static_cast<int>(i);
        }
    }
    for (int n = tail_root; n >= 0;) {
        rig.tail.push_back(n);
        int next = -1;
        for (const int c : children[static_cast<std::size_t>(n)]) {
            if (bone[static_cast<std::size_t>(c)]) {
                next = c;
                break;
            }
        }
        n = next;
    }

    rig.valid = rig.legs.size() >= 2 || rig.head >= 0;
    return rig;
}

std::vector<RagdollBone> ragdollBones(const std::vector<asset::Node>& nodes, const std::vector<Mat4>& rest,
                                      int max_bones) {
    std::vector<RagdollBone> out;
    if (nodes.empty() || rest.size() != nodes.size()) return out;
    struct Pick {
        int node;
        float radius;  // fraccion del alto
        float mass;
        float swing;
        float twist;
        float length;  // fraccion del alto si el hueso no tiene hijo (0 = automatico)
    };
    std::vector<Pick> picks;
    float height = 1.0f;

    const humanoid::Map human = humanoid::detect(nodes);
    if (human.valid) {
        using humanoid::Bone;
        const Rig rig = detect(nodes, rest);
        height = rig.height;
        const auto add = [&](Bone b, float radius, float mass, float swing, float twist, float length = 0.0f) {
            if (human[b] >= 0) picks.push_back({human[b], radius, mass, swing, twist, length});
        };
        add(Bone::Hips, 0.07f, 0.16f, 0.0f, 0.0f);
        add(Bone::Spine, 0.075f, 0.14f, 20.0f, 15.0f);
        add(human[Bone::UpperChest] >= 0 ? Bone::UpperChest : Bone::Chest, 0.08f, 0.16f, 20.0f, 15.0f);
        add(Bone::Head, 0.055f, 0.08f, 40.0f, 35.0f, 0.13f);
        add(Bone::LeftUpperArm, 0.028f, 0.03f, 80.0f, 40.0f);
        add(Bone::LeftLowerArm, 0.024f, 0.02f, 75.0f, 10.0f);
        add(Bone::RightUpperArm, 0.028f, 0.03f, 80.0f, 40.0f);
        add(Bone::RightLowerArm, 0.024f, 0.02f, 75.0f, 10.0f);
        add(Bone::LeftUpperLeg, 0.045f, 0.1f, 60.0f, 20.0f);
        add(Bone::LeftLowerLeg, 0.035f, 0.05f, 75.0f, 5.0f);
        add(Bone::RightUpperLeg, 0.045f, 0.1f, 60.0f, 20.0f);
        add(Bone::RightLowerLeg, 0.035f, 0.05f, 75.0f, 5.0f);
    } else {
        const Rig rig = detect(nodes, rest);
        if (!rig.valid) return out;
        height = rig.height;
        const auto length_of = [&](int n) { return boneLength(nodes, rest, n, 0.0f); };
        const float tiny = height * 0.03f;
        // Unos pocos de una lista larga, repartidos (columna, cola).
        const auto sample = [](const std::vector<int>& list, std::size_t max) {
            if (list.size() <= max) return list;
            std::vector<int> s;
            for (std::size_t k = 0; k < max; ++k) s.push_back(list[k * (list.size() - 1) / (max - 1)]);
            return s;
        };
        if (rig.body >= 0) picks.push_back({rig.body, 0.12f, 0.3f, 0.0f, 0.0f, 0.0f});
        for (const int n : sample(rig.spine, 3)) {
            if (length_of(n) >= tiny) picks.push_back({n, 0.1f, 0.2f / 3.0f, 15.0f, 10.0f, 0.0f});
        }
        for (const int n : sample(rig.neck, 2)) picks.push_back({n, 0.05f, 0.03f, 30.0f, 20.0f, 0.0f});
        if (rig.head >= 0) picks.push_back({rig.head, 0.06f, 0.06f, 30.0f, 20.0f, 0.12f});
        for (const Leg& leg : rig.legs) {
            std::vector<int> chain = [&] {
                std::vector<int> c;
                int n = nodes[static_cast<std::size_t>(leg.foot)].parent;
                for (int k = 0; k < leg.bones && n >= 0; ++k, n = nodes[static_cast<std::size_t>(n)].parent) c.push_back(n);
                std::reverse(c.begin(), c.end());
                return c;
            }();
            for (std::size_t k = 0; k < chain.size(); ++k) {
                if (length_of(chain[k]) < tiny) continue;
                picks.push_back({chain[k], k == 0 ? 0.035f : 0.025f, 0.3f / static_cast<float>(rig.legs.size() * chain.size()),
                                 50.0f, 10.0f, 0.0f});
            }
        }
        const std::vector<int> tail = sample(rig.tail, 3);
        for (std::size_t k = 0; k < tail.size(); ++k) {
            if (length_of(tail[k]) < tiny && k + 1 < tail.size()) continue;
            picks.push_back({tail[k], 0.03f * (1.0f - 0.25f * static_cast<float>(k)), 0.08f / 3.0f, 35.0f, 20.0f, 0.05f});
        }
    }

    // Padres antes que hijos (el orden de los nodos) y sin repetir.
    std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.node < b.node; });
    picks.erase(std::unique(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.node == b.node; }),
                picks.end());
    if (static_cast<int>(picks.size()) > max_bones) picks.resize(static_cast<std::size_t>(max_bones));

    float mass_sum = 0.0f;
    for (const Pick& pick : picks) mass_sum += pick.mass;
    for (const Pick& pick : picks) {
        RagdollBone b;
        b.node = pick.node;
        // Padre: el antepasado mas cercano que esta en la lista.
        for (int a = nodes[static_cast<std::size_t>(pick.node)].parent; a >= 0 && b.parent < 0;
             a = nodes[static_cast<std::size_t>(a)].parent) {
            for (std::size_t k = 0; k < out.size(); ++k) {
                if (out[k].node == a) {
                    b.parent = static_cast<int>(k);
                    break;
                }
            }
        }
        float length = 0.0f;  // se calcula abajo, con la lista completa
        if (length <= 0.0f) length = boneLength(nodes, rest, pick.node, 0.0f);
        if (pick.length > 0.0f && length <= height * 0.01f) length = pick.length * height;
        if (length <= 0.0f) length = height * 0.08f;
        b.length = length;
        b.radius = pick.radius * height;
        b.mass = mass_sum > 0.0f ? pick.mass / mass_sum : 1.0f / static_cast<float>(picks.size());
        b.swing = pick.swing;
        b.twist = pick.twist;
        out.push_back(b);
    }

    // Largo: hasta el hijo de la lista que sigue la direccion del hueso (la
    // raiz: hacia el que tiene mas descendientes, la columna). Sin hijos en
    // la lista, hasta su primer hijo del esqueleto.
    std::vector<int> descendants(out.size(), 0);
    for (std::size_t k = out.size(); k-- > 0;) {
        if (out[k].parent >= 0) descendants[static_cast<std::size_t>(out[k].parent)] += descendants[k] + 1;
    }
    for (std::size_t k = 0; k < out.size(); ++k) {
        const Vec3 from = position(rest[static_cast<std::size_t>(out[k].node)]);
        Vec3 along{};
        if (out[k].parent >= 0) {
            along = from - position(rest[static_cast<std::size_t>(out[static_cast<std::size_t>(out[k].parent)].node)]);
        }
        int best = -1;
        float score = -1e30f;
        for (std::size_t c = 0; c < out.size(); ++c) {
            if (out[c].parent != static_cast<int>(k)) continue;
            const Vec3 d = position(rest[static_cast<std::size_t>(out[c].node)]) - from;
            const float s = out[k].parent < 0 || core::length(along) < 1e-6f
                                ? static_cast<float>(descendants[c])
                                : core::dot(core::normalize(d), core::normalize(along));
            if (s > score) {
                score = s;
                best = static_cast<int>(c);
            }
        }
        if (best >= 0) {
            const float d = core::length(position(rest[static_cast<std::size_t>(out[static_cast<std::size_t>(best)].node)]) - from);
            if (d > height * 0.01f) {
                out[k].length = d;
                out[k].tip = out[static_cast<std::size_t>(best)].node;
            }
        }
        // Una capsula no es mas gorda que larga (brazos finos, cuellos cortos).
        out[k].radius = std::min(out[k].radius, std::max(out[k].length * 0.5f, height * 0.01f));
    }
    return out;
}

}  // namespace cramion::creature
