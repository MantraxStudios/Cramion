#include "CramionCore/ecs/AnimatorController.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace cramion::ecs {

using nlohmann::json;

namespace {

constexpr const char* kParameterTypes[] = {"float", "int", "bool", "trigger"};
constexpr const char* kConditionModes[] = {"if", "if_not", "greater", "less", "equals", "not_equals"};

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
        states.push_back({{"name", s.name},
                          {"clip_name", s.clip_name},
                          {"clip", s.clip.valid() ? s.clip.uuid.toString() : std::string{}},
                          {"speed", s.speed},
                          {"loop", s.loop},
                          {"position", vec2(s.position)}});
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
        return true;
    }
    const float normalized = clip_duration > 0.0f ? runtime.state_time / clip_duration : 1.0f;

    for (const AnimatorTransition& t : controller.transitions) {
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
        return true;
    }
    return false;
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
