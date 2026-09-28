#include "CramionCore/ecs/AnimatorController.h"

#include "CramionCore/anim/Humanoid.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

namespace cramion::ecs {

using nlohmann::json;

namespace {

constexpr const char* kParameterTypes[] = {"float", "int", "bool", "trigger"};
constexpr const char* kConditionModes[] = {"if", "if_not", "greater", "less", "equals", "not_equals"};
constexpr const char* kMotions[] = {"clip", "blend_1d", "blend_2d"};

template <std::size_t N>
int indexOf(const char* const (&names)[N], const std::string& text, int fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (text == names[i]) return static_cast<int>(i);
    }
    return fallback;
}

json vec2(const core::Vec2& v) {
    return json::array({v.x, v.y});
}

core::Vec2 readVec2(const json& j, core::Vec2 fallback) {
    if (j.is_array() && j.size() == 2 && j[0].is_number() && j[1].is_number()) {
        return core::Vec2{j[0].get<float>(), j[1].get<float>()};
    }
    return fallback;
}

// Escritura atomica: .tmp y renombrar (no deja el archivo a medias).
bool writeText(const std::filesystem::path& path, const std::string& text, std::string* error) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary);
        if (!file) {
            if (error) *error = "no se pudo escribir " + path.string();
            return false;
        }
        file << text;
        if (!file) {
            if (error) *error = "error escribiendo " + path.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

}  // namespace

const AnimatorParameter* AnimatorController::findParameter(const std::string& name) const {
    for (const AnimatorParameter& p : parameters) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

void AnimatorController::removeState(int index) {
    if (index < 0 || index >= static_cast<int>(states.size())) return;
    states.erase(states.begin() + index);
    std::vector<AnimatorTransition> kept;
    for (AnimatorTransition t : transitions) {
        if (t.from == index || t.to == index) continue;
        if (t.from > index) --t.from;
        if (t.to > index) --t.to;
        kept.push_back(std::move(t));
    }
    transitions = std::move(kept);
    if (default_state == index) default_state = 0;
    else if (default_state > index) --default_state;
    default_state = std::clamp(default_state, 0, std::max(0, static_cast<int>(states.size()) - 1));
}

// --- .cranimator -------------------------------------------------------------

bool saveAnimatorController(const AnimatorController& c, const std::filesystem::path& path,
                            std::string* error) {
    json root;
    root["uuid"] = (c.uuid.valid() ? c.uuid : Uuid::generate()).toString();
    root["type"] = "animator_controller";
    root["version"] = 1;
    root["default_state"] = c.default_state;
    root["entry"] = vec2(c.entry_position);
    root["any_state"] = vec2(c.any_state_position);
    json& parameters = root["parameters"] = json::array();
    for (const AnimatorParameter& p : c.parameters) {
        parameters.push_back({{"name", p.name},
                              {"type", kParameterTypes[static_cast<int>(p.type)]},
                              {"default", p.default_value}});
    }
    json& states = root["states"] = json::array();
    for (const AnimatorState& s : c.states) {
        json state = {{"name", s.name},
                      {"clip_name", s.clip_name},
                      {"clip", s.clip.valid() ? s.clip.uuid.toString() : std::string{}},
                      {"speed", s.speed},
                      {"loop", s.loop},
                      {"position", vec2(s.position)}};
        if (s.isBlendTree()) {
            state["motion"] = kMotions[static_cast<int>(s.motion)];
            state["blend_parameter"] = s.blend_parameter;
            state["blend_parameter_y"] = s.blend_parameter_y;
            json children = json::array();
            for (const BlendTreeChild& child : s.children) {
                children.push_back({{"clip_name", child.clip_name},
                                    {"clip", child.clip.valid() ? child.clip.uuid.toString() : std::string{}},
                                    {"threshold", child.threshold},
                                    {"position", vec2(child.position)},
                                    {"speed", child.speed}});
            }
            state["children"] = std::move(children);
        }
        states.push_back(std::move(state));
    }
    json& transitions = root["transitions"] = json::array();
    for (const AnimatorTransition& t : c.transitions) {
        json conditions = json::array();
        for (const AnimatorCondition& k : t.conditions) {
            conditions.push_back({{"parameter", k.parameter},
                                  {"mode", kConditionModes[static_cast<int>(k.mode)]},
                                  {"threshold", k.threshold}});
        }
        transitions.push_back({{"from", t.from},
                               {"to", t.to},
                               {"has_exit_time", t.has_exit_time},
                               {"exit_time", t.exit_time},
                               {"duration", t.duration},
                               {"blend", t.inertial ? "inertial" : "crossfade"},
                               {"sync_phase", t.sync_phase},
                               {"conditions", std::move(conditions)}});
    }
    return writeText(path, root.dump(2), error);
}

bool loadAnimatorController(const std::filesystem::path& path, AnimatorController& out,
                            std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + path.string();
        return false;
    }
    const json root = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        if (error) *error = "JSON danado en " + path.string();
        return false;
    }
    AnimatorController c;
    c.uuid = Uuid::parse(root.value("uuid", std::string{}));
    c.default_state = root.value("default_state", 0);
    if (const auto it = root.find("entry"); it != root.end()) c.entry_position = readVec2(*it, c.entry_position);
    if (const auto it = root.find("any_state"); it != root.end()) {
        c.any_state_position = readVec2(*it, c.any_state_position);
    }
    if (const auto it = root.find("parameters"); it != root.end() && it->is_array()) {
        for (const json& j : *it) {
            if (!j.is_object()) continue;
            AnimatorParameter p;
            p.name = j.value("name", std::string{});
            p.type = static_cast<AnimatorParameterType>(indexOf(kParameterTypes, j.value("type", std::string{}), 0));
            p.default_value = j.value("default", 0.0f);
            c.parameters.push_back(std::move(p));
        }
    }
    if (const auto it = root.find("states"); it != root.end() && it->is_array()) {
        for (const json& j : *it) {
            if (!j.is_object()) continue;
            AnimatorState s;
            s.name = j.value("name", std::string{"Estado"});
            s.clip_name = j.value("clip_name", std::string{});
            s.clip.uuid = Uuid::parse(j.value("clip", std::string{}));
            s.speed = j.value("speed", 1.0f);
            s.loop = j.value("loop", true);
            if (const auto p = j.find("position"); p != j.end()) s.position = readVec2(*p, s.position);
            s.motion = static_cast<AnimatorMotion>(indexOf(kMotions, j.value("motion", std::string{"clip"}), 0));
            s.blend_parameter = j.value("blend_parameter", std::string{});
            s.blend_parameter_y = j.value("blend_parameter_y", std::string{});
            if (const auto k = j.find("children"); k != j.end() && k->is_array()) {
                for (const json& jc : *k) {
                    if (!jc.is_object()) continue;
                    BlendTreeChild child;
                    child.clip_name = jc.value("clip_name", std::string{});
                    child.clip.uuid = Uuid::parse(jc.value("clip", std::string{}));
                    child.threshold = jc.value("threshold", 0.0f);
                    if (const auto p = jc.find("position"); p != jc.end()) child.position = readVec2(*p, child.position);
                    child.speed = jc.value("speed", 1.0f);
                    s.children.push_back(std::move(child));
                }
            }
            c.states.push_back(std::move(s));
        }
    }
    const int count = static_cast<int>(c.states.size());
    if (const auto it = root.find("transitions"); it != root.end() && it->is_array()) {
        for (const json& j : *it) {
            if (!j.is_object()) continue;
            AnimatorTransition t;
            t.from = j.value("from", 0);
            t.to = j.value("to", 0);
            if (t.to < 0 || t.to >= count || t.from < kAnyState || t.from >= count) continue;
            t.has_exit_time = j.value("has_exit_time", false);
            t.exit_time = j.value("exit_time", 1.0f);
            t.duration = std::max(0.0f, j.value("duration", 0.0f));
            t.inertial = j.value("blend", std::string{"inertial"}) != "crossfade";
            t.sync_phase = j.value("sync_phase", false);
            if (const auto k = j.find("conditions"); k != j.end() && k->is_array()) {
                for (const json& jc : *k) {
                    if (!jc.is_object()) continue;
                    AnimatorCondition condition;
                    condition.parameter = jc.value("parameter", std::string{});
                    condition.mode = static_cast<AnimatorConditionMode>(
                        indexOf(kConditionModes, jc.value("mode", std::string{}), 0));
                    condition.threshold = jc.value("threshold", 0.0f);
                    t.conditions.push_back(std::move(condition));
                }
            }
            c.transitions.push_back(std::move(t));
        }
    }
    c.default_state = std::clamp(c.default_state, 0, std::max(0, count - 1));
    out = std::move(c);
    return true;
}

// --- Maquina de estados --------------------------------------------------------

float animatorParameterValue(const AnimatorController& controller, const AnimatorRuntime& runtime,
                             const std::string& name) {
    if (const auto it = runtime.values.find(name); it != runtime.values.end()) return it->second;
    if (const AnimatorParameter* p = controller.findParameter(name)) {
        return p->type == AnimatorParameterType::Trigger ? 0.0f : p->default_value;
    }
    return 0.0f;
}

bool stepAnimatorController(const AnimatorController& controller, AnimatorRuntime& runtime,
                            float clip_duration) {
    const int count = static_cast<int>(controller.states.size());
    if (count == 0) {
        runtime.state = -1;
        return false;
    }
    if (runtime.state < 0 || runtime.state >= count) {
        runtime.state = std::clamp(controller.default_state, 0, count - 1);
        runtime.state_time = 0.0f;
        runtime.last_transition = -1;
        return true;
    }
    const float normalized = clip_duration > 0.0f ? runtime.state_time / clip_duration : 1.0f;

    for (std::size_t ti = 0; ti < controller.transitions.size(); ++ti) {
        const AnimatorTransition& t = controller.transitions[ti];
        if (t.from != runtime.state && t.from != kAnyState) continue;
        if (t.from == kAnyState && t.to == runtime.state) continue;  // no reentra en bucle
        if (t.to < 0 || t.to >= count) continue;
        // Sin condiciones ni exit time se dispararia siempre: se ignora.
        if (t.conditions.empty() && !t.has_exit_time) continue;
        if (t.has_exit_time) {
            // En bucle (exit time < 1): se sale en cada vuelta al pasar el punto.
            const bool loops = controller.states[runtime.state].loop && t.exit_time < 1.0f;
            const float phase = loops ? normalized - std::floor(normalized) : normalized;
            if (phase < t.exit_time) continue;
        }
        bool ok = true;
        for (const AnimatorCondition& k : t.conditions) {
            const float value = animatorParameterValue(controller, runtime, k.parameter);
            switch (k.mode) {
                case AnimatorConditionMode::If: ok = value != 0.0f; break;
                case AnimatorConditionMode::IfNot: ok = value == 0.0f; break;
                case AnimatorConditionMode::Greater: ok = value > k.threshold; break;
                case AnimatorConditionMode::Less: ok = value < k.threshold; break;
                case AnimatorConditionMode::Equals: ok = std::lround(value) == std::lround(k.threshold); break;
                case AnimatorConditionMode::NotEquals: ok = std::lround(value) != std::lround(k.threshold); break;
            }
            if (!ok) break;
        }
        if (!ok) continue;
        // Los triggers usados se consumen.
        for (const AnimatorCondition& k : t.conditions) {
            const AnimatorParameter* p = controller.findParameter(k.parameter);
            if (p != nullptr && p->type == AnimatorParameterType::Trigger) runtime.values[k.parameter] = 0.0f;
        }
        runtime.state = t.to;
        runtime.state_time = 0.0f;
        runtime.last_transition = static_cast<int>(ti);
        return true;
    }
    return false;
}

// --- Blend Trees -----------------------------------------------------------------

std::vector<float> blendTreeWeights(const AnimatorState& state, float x, float y) {
    const std::size_t n = state.children.size();
    std::vector<float> weights(n, 0.0f);
    if (n == 0) return weights;
    if (n == 1) {
        weights[0] = 1.0f;
        return weights;
    }
    if (state.motion == AnimatorMotion::BlendTree2D) {
        // Gradient Band: w_i = min_j (1 - dot(p - p_i, p_j - p_i) / |p_j - p_i|^2), recortado a [0, 1].
        float total = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const core::Vec2 pi = state.children[i].position;
            float w = 1.0f;
            for (std::size_t j = 0; j < n && w > 0.0f; ++j) {
                if (j == i) continue;
                const core::Vec2 pj = state.children[j].position;
                const float ex = pj.x - pi.x, ey = pj.y - pi.y;
                const float len2 = ex * ex + ey * ey;
                if (len2 < 1e-8f) continue;  // dos en el mismo punto: se reparten
                const float h = 1.0f - ((x - pi.x) * ex + (y - pi.y) * ey) / len2;
                w = std::min(w, std::clamp(h, 0.0f, 1.0f));
            }
            weights[i] = w;
            total += w;
        }
        if (total <= 1e-6f) {
            // Fuera de todo (no deberia pasar): el mas cercano.
            std::size_t best = 0;
            float best_d = 1e30f;
            for (std::size_t i = 0; i < n; ++i) {
                const float dx = x - state.children[i].position.x, dy = y - state.children[i].position.y;
                if (dx * dx + dy * dy < best_d) {
                    best_d = dx * dx + dy * dy;
                    best = i;
                }
            }
            std::fill(weights.begin(), weights.end(), 0.0f);
            weights[best] = 1.0f;
            return weights;
        }
        for (float& w : weights) w /= total;
        return weights;
    }
    // 1D: orden por umbral; entre los dos vecinos, lineal.
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return state.children[a].threshold < state.children[b].threshold; });
    if (x <= state.children[order.front()].threshold) {
        weights[order.front()] = 1.0f;
        return weights;
    }
    if (x >= state.children[order.back()].threshold) {
        weights[order.back()] = 1.0f;
        return weights;
    }
    for (std::size_t k = 0; k + 1 < n; ++k) {
        const float a = state.children[order[k]].threshold;
        const float b = state.children[order[k + 1]].threshold;
        if (x >= a && x <= b) {
            const float t = b - a > 1e-6f ? (x - a) / (b - a) : 0.0f;
            weights[order[k]] = 1.0f - t;
            weights[order[k + 1]] = t;
            break;
        }
    }
    return weights;
}

// --- .cranim -------------------------------------------------------------------

bool saveAnimationClip(const asset::ModelData& model, const asset::AnimationClip& clip,
                       const std::filesystem::path& path, std::string* error) {
    json header;
    header["uuid"] = Uuid::generate().toString();
    header["type"] = "animation_clip";
    header["version"] = 1;
    header["name"] = clip.name;
    header["duration"] = clip.duration;
    header["channels"] = clip.channels.size();
    // Humanoide: se puede usar en otro humanoide (se convierte al cargarlo).
    header["humanoid"] = humanoid::isHumanoid(model.nodes);

    json channels = json::array();
    for (const asset::AnimationChannel& channel : clip.channels) {
        if (channel.node < 0 || static_cast<std::size_t>(channel.node) >= model.nodes.size()) continue;
        json p = json::array();
        for (const asset::VectorKey& k : channel.positions) p.push_back({k.time, k.value.x, k.value.y, k.value.z});
        json r = json::array();
        for (const asset::QuatKey& k : channel.rotations) {
            r.push_back({k.time, k.value.x, k.value.y, k.value.z, k.value.w});
        }
        json s = json::array();
        for (const asset::VectorKey& k : channel.scales) s.push_back({k.time, k.value.x, k.value.y, k.value.z});
        channels.push_back({{"node", model.nodes[channel.node].name}, {"p", std::move(p)}, {"r", std::move(r)},
                            {"s", std::move(s)}});
    }
    json body;
    body["channels"] = std::move(channels);
    // El esqueleto de origen (nombres, jerarquia y reposo): hace falta para
    // pasar el clip a otro esqueleto humanoide con otros nombres.
    json skeleton = json::array();
    for (const asset::Node& node : model.nodes) {
        json m = json::array();
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) m.push_back(node.local.m[c][r]);
        }
        skeleton.push_back({{"n", node.name}, {"p", node.parent}, {"m", std::move(m)}});
    }
    body["skeleton"] = std::move(skeleton);
    // Linea 1: cabecera (la lee la base de datos); linea 2: las pistas.
    return writeText(path, header.dump() + "\n" + body.dump() + "\n", error);
}

bool loadAnimationClip(const std::filesystem::path& path, const asset::ModelData& model,
                       asset::AnimationClip& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + path.string();
        return false;
    }
    std::string header_line;
    std::getline(file, header_line);
    const json header = json::parse(header_line, nullptr, false);
    const json body = json::parse(file, nullptr, false);
    if (header.is_discarded() || body.is_discarded() || !body.contains("channels")) {
        if (error) *error = "clip danado: " + path.string();
        return false;
    }
    std::unordered_map<std::string, int> nodes;
    for (std::size_t i = 0; i < model.nodes.size(); ++i) nodes.emplace(model.nodes[i].name, static_cast<int>(i));

    // Humanoide con otros nombres de huesos: se convierte (retargeting).
    std::size_t matched = 0;
    for (const json& j : body["channels"]) {
        if (nodes.contains(j.value("node", std::string{}))) ++matched;
    }
    const std::size_t total = body["channels"].size();
    if (total > 0 && matched * 10 < total * 8 && body.contains("skeleton") && body["skeleton"].is_array()) {
        std::vector<asset::Node> source;
        std::unordered_map<std::string, int> source_index;
        for (const json& n : body["skeleton"]) {
            asset::Node node;
            node.name = n.value("n", std::string{});
            node.parent = n.value("p", -1);
            if (const auto m = n.find("m"); m != n.end() && m->is_array() && m->size() == 16) {
                for (int c = 0; c < 4; ++c) {
                    for (int r = 0; r < 4; ++r) node.local.m[c][r] = (*m)[static_cast<std::size_t>(c * 4 + r)].get<float>();
                }
            }
            source_index.emplace(node.name, static_cast<int>(source.size()));
            source.push_back(std::move(node));
        }
        asset::AnimationClip source_clip;
        source_clip.name = header.value("name", std::string{});
        source_clip.duration = header.value("duration", 0.0f);
        for (const json& j : body["channels"]) {
            const auto node = source_index.find(j.value("node", std::string{}));
            if (node == source_index.end()) continue;
            asset::AnimationChannel channel;
            channel.node = node->second;
            for (const json& k : j.value("p", json::array())) {
                if (k.size() == 4) channel.positions.push_back({k[0].get<float>(), {k[1].get<float>(), k[2].get<float>(), k[3].get<float>()}});
            }
            for (const json& k : j.value("r", json::array())) {
                if (k.size() == 5) {
                    channel.rotations.push_back(
                        {k[0].get<float>(), {k[1].get<float>(), k[2].get<float>(), k[3].get<float>(), k[4].get<float>()}});
                }
            }
            for (const json& k : j.value("s", json::array())) {
                if (k.size() == 4) channel.scales.push_back({k[0].get<float>(), {k[1].get<float>(), k[2].get<float>(), k[3].get<float>()}});
            }
            source_clip.channels.push_back(std::move(channel));
        }
        std::string why;
        if (humanoid::retargetClip(source, source_clip, model.nodes, out, &why)) {
            std::cout << "[Animacion] " << source_clip.name << ": convertido de otro humanoide ("
                      << out.channels.size() << " huesos)\n";
            return true;
        }
        // No son humanoides: sigue con lo que coincida por nombre.
        if (error) *error = why;
    }

    asset::AnimationClip clip;
    clip.name = header.value("name", std::string{});
    clip.duration = header.value("duration", 0.0f);
    for (const json& j : body["channels"]) {
        const auto node = nodes.find(j.value("node", std::string{}));
        if (node == nodes.end()) continue;
        asset::AnimationChannel channel;
        channel.node = node->second;
        for (const json& k : j.value("p", json::array())) {
            if (k.size() == 4) channel.positions.push_back({k[0].get<float>(), {k[1].get<float>(), k[2].get<float>(), k[3].get<float>()}});
        }
        for (const json& k : j.value("r", json::array())) {
            if (k.size() == 5) {
                channel.rotations.push_back(
                    {k[0].get<float>(), {k[1].get<float>(), k[2].get<float>(), k[3].get<float>(), k[4].get<float>()}});
            }
        }
        for (const json& k : j.value("s", json::array())) {
            if (k.size() == 4) channel.scales.push_back({k[0].get<float>(), {k[1].get<float>(), k[2].get<float>(), k[3].get<float>()}});
        }
        clip.channels.push_back(std::move(channel));
    }
    out = std::move(clip);
    return true;
}

}  // namespace cramion::ecs
