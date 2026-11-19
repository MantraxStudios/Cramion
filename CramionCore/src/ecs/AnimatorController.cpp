#include "CramionCore/ecs/AnimatorController.h"

#include "CramionCore/anim/Humanoid.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
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
    removeAnimatorState(states, transitions, default_state, index);
}

// --- .cranimator -------------------------------------------------------------

namespace {

json statesToJson(const std::vector<AnimatorState>& list) {
    json states = json::array();
    for (const AnimatorState& s : list) {
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
    return states;
}

json transitionsToJson(const std::vector<AnimatorTransition>& list) {
    json transitions = json::array();
    for (const AnimatorTransition& t : list) {
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
    return transitions;
}

std::vector<AnimatorState> statesFromJson(const json& root) {
    std::vector<AnimatorState> out;
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
            out.push_back(std::move(s));
        }
    }
    return out;
}

std::vector<AnimatorTransition> transitionsFromJson(const json& root, int count) {
    std::vector<AnimatorTransition> out;
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
            out.push_back(std::move(t));
        }
    }
    return out;
}

json maskEntriesToJson(const std::vector<BoneMaskEntry>& entries);
std::vector<BoneMaskEntry> maskEntriesFromJson(const json& j);

}  // namespace

bool saveAnimatorController(const AnimatorController& c, const std::filesystem::path& path,
                            std::string* error) {
    json root;
    root["uuid"] = (c.uuid.valid() ? c.uuid : Uuid::generate()).toString();
    root["type"] = "animator_controller";
    // Version 2 solo con capas: un controlador sin ellas se guarda igual que antes.
    root["version"] = c.layers.empty() ? 1 : 2;
    root["default_state"] = c.default_state;
    root["entry"] = vec2(c.entry_position);
    root["any_state"] = vec2(c.any_state_position);
    json& parameters = root["parameters"] = json::array();
    for (const AnimatorParameter& p : c.parameters) {
        parameters.push_back({{"name", p.name},
                              {"type", kParameterTypes[static_cast<int>(p.type)]},
                              {"default", p.default_value}});
    }
    root["states"] = statesToJson(c.states);
    root["transitions"] = transitionsToJson(c.transitions);
    if (!c.layers.empty()) {
        if (c.base_layer_name != "Base") root["base_layer"] = c.base_layer_name;
        json layers = json::array();
        for (const AnimatorLayer& l : c.layers) {
            json layer = {{"name", l.name},
                          {"weight", l.weight},
                          {"blending", l.blending == AnimatorLayerBlend::Additive ? "additive" : "override"},
                          {"mask", l.mask.valid() ? l.mask.uuid.toString() : std::string{}},
                          {"mask_bones", maskEntriesToJson(l.mask_bones)},
                          {"sync_layer", l.sync_layer},
                          {"default_state", l.default_state},
                          {"entry", vec2(l.entry_position)},
                          {"any_state", vec2(l.any_state_position)},
                          {"states", statesToJson(l.states)},
                          {"transitions", transitionsToJson(l.transitions)}};
            if (!l.reference_clip_name.empty() || l.reference_clip.valid()) {
                layer["reference_clip_name"] = l.reference_clip_name;
                layer["reference_clip"] = l.reference_clip.valid() ? l.reference_clip.uuid.toString() : std::string{};
                layer["reference_time"] = l.reference_time;
            }
            layers.push_back(std::move(layer));
        }
        root["layers"] = std::move(layers);
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
    c.states = statesFromJson(root);
    const int count = static_cast<int>(c.states.size());
    c.transitions = transitionsFromJson(root, count);
    c.base_layer_name = root.value("base_layer", std::string{"Base"});
    if (const auto it = root.find("layers"); it != root.end() && it->is_array()) {
        for (const json& j : *it) {
            if (!j.is_object()) continue;
            AnimatorLayer l;
            l.name = j.value("name", std::string{"Capa"});
            l.weight = std::clamp(j.value("weight", 1.0f), 0.0f, 1.0f);
            l.blending = j.value("blending", std::string{"override"}) == "additive" ? AnimatorLayerBlend::Additive
                                                                                   : AnimatorLayerBlend::Override;
            l.mask.uuid = Uuid::parse(j.value("mask", std::string{}));
            if (const auto m = j.find("mask_bones"); m != j.end()) l.mask_bones = maskEntriesFromJson(*m);
            l.reference_clip_name = j.value("reference_clip_name", std::string{});
            l.reference_clip.uuid = Uuid::parse(j.value("reference_clip", std::string{}));
            l.reference_time = j.value("reference_time", 0.0f);
            l.states = statesFromJson(j);
            const int layer_count = static_cast<int>(l.states.size());
            l.transitions = transitionsFromJson(j, layer_count);
            l.default_state = std::clamp(j.value("default_state", 0), 0, std::max(0, layer_count - 1));
            if (const auto e = j.find("entry"); e != j.end()) l.entry_position = readVec2(*e, l.entry_position);
            if (const auto e = j.find("any_state"); e != j.end()) l.any_state_position = readVec2(*e, l.any_state_position);
            // Solo de una capa de antes (la base es la 0; esta sera la k+1).
            const int self = static_cast<int>(c.layers.size()) + 1;
            l.sync_layer = j.value("sync_layer", -1);
            if (l.sync_layer >= self || l.sync_layer < -1) l.sync_layer = -1;
            c.layers.push_back(std::move(l));
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
    return stepStateMachine(controller, controller.states, controller.transitions, controller.default_state, runtime,
                            runtime.state, runtime.state_time, runtime.last_transition, clip_duration);
}

// --- Blend Trees -----------------------------------------------------------------

std::vector<float> blendTreeWeights(const AnimatorState& state, float x, float y) {
    std::vector<float> weights;
    blendTreeWeights(state, x, y, weights);
    return weights;
}

void blendTreeWeights(const AnimatorState& state, float x, float y, std::vector<float>& weights) {
    const std::size_t n = state.children.size();
    weights.assign(n, 0.0f);
    if (n == 0) return;
    if (n == 1) {
        weights[0] = 1.0f;
        return;
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
            return;
        }
        for (float& w : weights) w /= total;
        return;
    }
    // 1D: orden por umbral; entre los dos vecinos, lineal. El orden se
    // reutiliza por hilo (antes, una reserva por estado y frame).
    thread_local std::vector<std::size_t> order;
    order.resize(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return state.children[a].threshold < state.children[b].threshold; });
    if (x <= state.children[order.front()].threshold) {
        weights[order.front()] = 1.0f;
        return;
    }
    if (x >= state.children[order.back()].threshold) {
        weights[order.back()] = 1.0f;
        return;
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
}

// --- .cranim -------------------------------------------------------------------

json clipMetaToJsonPublic(const ClipMeta& meta);
ClipMeta clipMetaFromJsonPublic(const json& j);

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
    // Eventos, curvas y secciones (si los tiene; los motores de antes lo ignoran).
    if (const ClipMeta meta = clipMetaOf(clip); !meta.empty()) header["meta"] = clipMetaToJsonPublic(meta);

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

namespace {
// Las claves de un canal (p, r, s) sin copiar sus arrays (json::value los copiaba enteros).
void readChannelKeys(const json& j, asset::AnimationChannel& channel) {
    if (const auto p = j.find("p"); p != j.end() && p->is_array()) {
        channel.positions.reserve(p->size());
        for (const json& k : *p) {
            if (k.size() == 4) channel.positions.push_back({k[0].get<float>(), {k[1].get<float>(), k[2].get<float>(), k[3].get<float>()}});
        }
    }
    if (const auto r = j.find("r"); r != j.end() && r->is_array()) {
        channel.rotations.reserve(r->size());
        for (const json& k : *r) {
            if (k.size() == 5) {
                channel.rotations.push_back({k[0].get<float>(), {k[1].get<float>(), k[2].get<float>(), k[3].get<float>(), k[4].get<float>()}});
            }
        }
    }
    if (const auto s = j.find("s"); s != j.end() && s->is_array()) {
        channel.scales.reserve(s->size());
        for (const json& k : *s) {
            if (k.size() == 4) channel.scales.push_back({k[0].get<float>(), {k[1].get<float>(), k[2].get<float>(), k[3].get<float>()}});
        }
    }
}
}  // namespace

bool loadAnimationClip(const std::filesystem::path& path, const asset::ModelData& model,
                       asset::AnimationClip& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + path.string();
        return false;
    }
    // Se lee entero y se analiza de memoria (el istream iba caracter a caracter).
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::size_t cut = text.find('\n');
    const json header = json::parse(text.begin(), cut == std::string::npos ? text.end() : text.begin() + static_cast<std::ptrdiff_t>(cut),
                                    nullptr, false);
    const json body = cut == std::string::npos ? json() : json::parse(text.begin() + static_cast<std::ptrdiff_t>(cut) + 1, text.end(), nullptr, false);
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
            readChannelKeys(j, channel);
            source_clip.channels.push_back(std::move(channel));
        }
        std::string why;
        if (humanoid::retargetClip(source, source_clip, model.nodes, out, &why)) {
            if (const auto m = header.find("meta"); m != header.end()) applyClipMeta(out, clipMetaFromJsonPublic(*m));
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
        readChannelKeys(j, channel);
        clip.channels.push_back(std::move(channel));
    }
    if (const auto m = header.find("meta"); m != header.end()) applyClipMeta(clip, clipMetaFromJsonPublic(*m));
    out = std::move(clip);
    return true;
}


// --- Mascaras de huesos ------------------------------------------------------------

bool boneNameMatches(const std::string& node, const std::string& bone) {
    if (bone.empty()) return false;
    if (node == bone) return true;
    // "mixamorig:Spine" o "Armature|Spine" frente a "Spine".
    if (node.size() > bone.size() && node.compare(node.size() - bone.size(), bone.size(), bone) == 0) {
        const char c = node[node.size() - bone.size() - 1];
        return c == ':' || c == '|';
    }
    return false;
}

std::vector<float> buildMaskWeights(const std::vector<asset::Node>& nodes, const std::vector<BoneMaskEntry>& entries) {
    std::vector<float> weights(nodes.size(), entries.empty() ? 1.0f : 0.0f);
    std::vector<std::uint8_t> inside(nodes.size(), 0);
    for (const BoneMaskEntry& entry : entries) {
        // Los padres van antes que sus hijos: una pasada basta para el subarbol.
        std::fill(inside.begin(), inside.end(), std::uint8_t{0});
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const bool self = boneNameMatches(nodes[i].name, entry.bone);
            const int parent = nodes[i].parent;
            const bool below = entry.children && parent >= 0 && static_cast<std::size_t>(parent) < nodes.size() &&
                               inside[static_cast<std::size_t>(parent)] != 0;
            if (self || below) {
                inside[i] = 1;
                weights[i] = std::clamp(entry.weight, 0.0f, 1.0f);
            }
        }
    }
    return weights;
}

namespace {
json maskEntriesToJson(const std::vector<BoneMaskEntry>& entries) {
    json out = json::array();
    for (const BoneMaskEntry& e : entries) {
        out.push_back({{"bone", e.bone}, {"children", e.children}, {"weight", e.weight}});
    }
    return out;
}

std::vector<BoneMaskEntry> maskEntriesFromJson(const json& j) {
    std::vector<BoneMaskEntry> out;
    if (!j.is_array()) return out;
    for (const json& e : j) {
        if (!e.is_object()) continue;
        BoneMaskEntry entry;
        entry.bone = e.value("bone", std::string{});
        entry.children = e.value("children", true);
        entry.weight = e.value("weight", 1.0f);
        if (!entry.bone.empty()) out.push_back(std::move(entry));
    }
    return out;
}
}  // namespace

bool loadBoneMask(const std::filesystem::path& path, BoneMask& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + path.string();
        return false;
    }
    const json root = json::parse(file, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        if (error) *error = "JSON danado en " + path.string();
        return false;
    }
    BoneMask mask;
    mask.uuid = Uuid::parse(root.value("uuid", std::string{}));
    if (const auto it = root.find("bones"); it != root.end()) mask.bones = maskEntriesFromJson(*it);
    out = std::move(mask);
    return true;
}

bool saveBoneMask(const BoneMask& mask, const std::filesystem::path& path, std::string* error) {
    json root;
    root["uuid"] = (mask.uuid.valid() ? mask.uuid : Uuid::generate()).toString();
    root["type"] = "bone_mask";
    root["version"] = 1;
    root["bones"] = maskEntriesToJson(mask.bones);
    return writeText(path, root.dump(2), error);
}

// --- Maquinas de estados y capas ------------------------------------------------------

int AnimatorController::findLayer(const std::string& name) const {
    if (name == base_layer_name) return 0;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].name == name) return static_cast<int>(i) + 1;
    }
    return -1;
}

void removeAnimatorState(std::vector<AnimatorState>& states, std::vector<AnimatorTransition>& transitions,
                         int& default_state, int index) {
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

float animatorStateSamples(const AnimatorController& controller, const AnimatorRuntime& runtime,
                           const AnimatorState& st, float phase, float dphase, float weight,
                           const AnimatorClipLookup& lookup, std::vector<anim::ClipSample>* out,
                           std::vector<float>& scratch) {
    if (!st.isBlendTree()) {
        const int c = lookup.resolve ? lookup.resolve(st.clip_name, st.clip) : -1;
        const float d = c >= 0 && lookup.duration ? lookup.duration(c) : 0.0f;
        if (out != nullptr) out->push_back({c, phase * d, weight, dphase * d, st.loop});
        return d;
    }
    const float px = animatorParameterValue(controller, runtime, st.blend_parameter);
    const float py = animatorParameterValue(controller, runtime, st.blend_parameter_y);
    blendTreeWeights(st, px, py, scratch);
    float cycle = 0.0f, used = 0.0f;
    const std::size_t first = out != nullptr ? out->size() : 0;
    for (std::size_t k = 0; k < st.children.size() && k < scratch.size(); ++k) {
        if (scratch[k] <= 1e-4f) continue;
        const BlendTreeChild& child = st.children[k];
        const int c = lookup.resolve ? lookup.resolve(child.clip_name, child.clip) : -1;
        if (c < 0) continue;
        const float clip_duration = lookup.duration ? lookup.duration(c) : 0.0f;
        cycle += scratch[k] * clip_duration / std::max(std::abs(child.speed), 0.01f);
        used += scratch[k];
        if (out != nullptr) {
            // Velocidad negativa: el clip va hacia atras (andar de espaldas).
            const bool backwards = child.speed < 0.0f;
            const float t = backwards ? 1.0f - phase : phase;
            out->push_back({c, t * clip_duration, scratch[k], (backwards ? -dphase : dphase) * clip_duration, st.loop});
        }
    }
    if (out != nullptr && used > 1e-6f) {
        for (std::size_t k = first; k < out->size(); ++k) (*out)[k].weight *= weight / used;
    }
    return used > 1e-6f ? cycle / used : 0.0f;
}

bool stepStateMachine(const AnimatorController& controller, const std::vector<AnimatorState>& states,
                      const std::vector<AnimatorTransition>& transitions, int default_state, AnimatorRuntime& params,
                      int& state, float& state_time, int& last_transition, float clip_duration) {
    const int count = static_cast<int>(states.size());
    if (count == 0) {
        state = -1;
        return false;
    }
    if (state < 0 || state >= count) {
        state = std::clamp(default_state, 0, count - 1);
        state_time = 0.0f;
        last_transition = -1;
        return true;
    }
    const float normalized = clip_duration > 0.0f ? state_time / clip_duration : 1.0f;

    for (std::size_t ti = 0; ti < transitions.size(); ++ti) {
        const AnimatorTransition& t = transitions[ti];
        if (t.from != state && t.from != kAnyState) continue;
        if (t.from == kAnyState && t.to == state) continue;  // no reentra en bucle
        if (t.to < 0 || t.to >= count) continue;
        // Sin condiciones ni exit time se dispararia siempre: se ignora.
        if (t.conditions.empty() && !t.has_exit_time) continue;
        if (t.has_exit_time) {
            // En bucle (exit time < 1): se sale en cada vuelta al pasar el punto.
            const bool loops = states[static_cast<std::size_t>(state)].loop && t.exit_time < 1.0f;
            const float phase = loops ? normalized - std::floor(normalized) : normalized;
            if (phase < t.exit_time) continue;
        }
        bool ok = true;
        for (const AnimatorCondition& k : t.conditions) {
            const float value = animatorParameterValue(controller, params, k.parameter);
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
            if (p != nullptr && p->type == AnimatorParameterType::Trigger) params.values[k.parameter] = 0.0f;
        }
        state = t.to;
        state_time = 0.0f;
        last_transition = static_cast<int>(ti);
        return true;
    }
    return false;
}

float animatorLayerWeight(const AnimatorController& controller, const AnimatorRuntime& runtime, int layer) {
    if (layer <= 0) return 1.0f;
    const std::size_t i = static_cast<std::size_t>(layer - 1);
    if (i < runtime.layers.size() && runtime.layers[i].weight >= 0.0f) return std::clamp(runtime.layers[i].weight, 0.0f, 1.0f);
    return i < controller.layers.size() ? std::clamp(controller.layers[i].weight, 0.0f, 1.0f) : 0.0f;
}

namespace {
float smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
}  // namespace

void stepAnimatorLayer(const AnimatorController& controller, int layer, AnimatorRuntime& runtime, float dt,
                       float speed, const AnimatorClipLookup& lookup, std::vector<anim::ClipSample>& out,
                       std::size_t& current_begin, const AnimatorSyncSource* sync, std::vector<float>& scratch) {
    out.clear();
    current_begin = 0;
    if (layer < 1 || layer > static_cast<int>(controller.layers.size())) return;
    const AnimatorLayer& def = controller.layers[static_cast<std::size_t>(layer - 1)];
    if (runtime.layers.size() < controller.layers.size()) runtime.layers.resize(controller.layers.size());
    AnimatorLayerRuntime& lr = runtime.layers[static_cast<std::size_t>(layer - 1)];
    lr.entered = false;

    // Sincronizada: el estado y la fase de la otra capa, con los clips de esta.
    if (sync != nullptr && def.sync_layer >= 0) {
        const std::vector<AnimatorState>& source_states =
            def.sync_layer == 0 ? controller.states : controller.layers[static_cast<std::size_t>(def.sync_layer - 1)].states;
        const auto motionOf = [&](int index) -> const AnimatorState* {
            if (index < 0) return nullptr;
            const std::size_t i = static_cast<std::size_t>(index);
            if (i < def.states.size()) {
                const AnimatorState& own = def.states[i];
                if (own.isBlendTree() || !own.clip_name.empty() || own.clip.valid()) return &own;
            }
            return i < source_states.size() ? &source_states[i] : nullptr;
        };
        lr.entered = sync->entered;
        lr.state = sync->state;
        lr.phase = sync->phase;
        lr.dphase = sync->dphase;
        lr.fade_state = sync->fade_state;
        lr.fade_phase = sync->fade_phase;
        lr.fade_weight = sync->fade_weight;
        if (const AnimatorState* st = motionOf(sync->state)) lr.state_name = st->name;
        if (sync->fade_weight > 1e-4f) {
            if (const AnimatorState* old = motionOf(sync->fade_state)) {
                animatorStateSamples(controller, runtime, *old, sync->fade_phase, sync->dphase, sync->fade_weight, lookup,
                                     &out, scratch);
            }
        }
        current_begin = out.size();
        if (const AnimatorState* st = motionOf(sync->state)) {
            animatorStateSamples(controller, runtime, *st, sync->phase, sync->dphase, 1.0f - sync->fade_weight, lookup,
                                 &out, scratch);
        }
        return;
    }

    const std::vector<AnimatorState>& states = def.states;
    const int count = static_cast<int>(states.size());
    if (count == 0) return;
    const auto cycleOf = [&](int index, float phase) {
        if (index < 0 || index >= count) return 0.0f;
        return animatorStateSamples(controller, runtime, states[static_cast<std::size_t>(index)], phase, 0.0f, 0.0f,
                                    lookup, nullptr, scratch);
    };
    const auto enter = [&](int index, float fade) {
        if (fade > 0.0f && lr.state >= 0) {
            lr.fade_state = lr.state;
            lr.fade_phase = lr.phase;
            lr.fade_time = 0.0f;
            lr.fade_duration = fade;
        } else {
            lr.fade_state = -1;
        }
        lr.state = index;
        lr.phase = 0.0f;
        lr.state_time = 0.0f;
        lr.entered = true;
    };

    // CrossFade pedido por el juego: manda sobre las transiciones.
    bool forced = false;
    if (!lr.cross_fade.empty()) {
        for (int i = 0; i < count; ++i) {
            if (states[static_cast<std::size_t>(i)].name == lr.cross_fade) {
                enter(i, std::max(lr.cross_fade_time, 0.0f));
                lr.last_transition = -1;
                forced = true;
                break;
            }
        }
        lr.cross_fade.clear();
    }
    if (!forced) {
        const int before = lr.state;
        int state = lr.state;
        float state_time = lr.state_time;
        int last = lr.last_transition;
        if (stepStateMachine(controller, states, def.transitions, def.default_state, runtime, state, state_time, last,
                             cycleOf(lr.state, lr.phase))) {
            lr.last_transition = last;
            const AnimatorTransition* t =
                before >= 0 && last >= 0 && last < static_cast<int>(def.transitions.size())
                    ? &def.transitions[static_cast<std::size_t>(last)]
                    : nullptr;
            const float old_phase = lr.phase;
            // Las capas mezclan siempre con fundido cruzado (la inercializacion
            // es de la base).
            enter(state, t != nullptr ? t->duration : 0.0f);
            if (t != nullptr && t->sync_phase) lr.phase = old_phase;
        }
    }
    if (lr.state < 0 || lr.state >= count) return;
    const AnimatorState& st = states[static_cast<std::size_t>(lr.state)];
    if (lr.state_name != st.name) lr.state_name = st.name;

    // Avanza la fase del estado actual (y la del que se va).
    const auto advance = [&](int index, float& phase) {
        const float cycle = cycleOf(index, phase);
        if (cycle <= 0.0f) return 0.0f;
        const AnimatorState& s = states[static_cast<std::size_t>(index)];
        const float step = dt * speed * s.speed / cycle;
        if (s.loop) {
            phase += step;
            phase -= std::floor(phase);
            return step;
        }
        // Sin bucle, lo que avanzo de verdad: parado al final no repite eventos.
        const float before = phase;
        phase = std::clamp(phase + step, 0.0f, 1.0f);
        return phase - before;
    };
    lr.state_time += dt * std::abs(speed * st.speed);
    const float dphase = advance(lr.state, lr.phase);
    float fresh = 1.0f;
    float fade_dphase = 0.0f;
    if (lr.fade_state >= 0) {
        fade_dphase = advance(lr.fade_state, lr.fade_phase);
        lr.fade_time += dt;
        if (lr.fade_time >= lr.fade_duration) {
            lr.fade_state = -1;
        } else {
            fresh = smooth(lr.fade_duration > 0.0f ? lr.fade_time / lr.fade_duration : 1.0f);
        }
    }
    lr.dphase = dphase;
    lr.fade_weight = lr.fade_state >= 0 ? 1.0f - fresh : 0.0f;
    if (lr.fade_state >= 0) {
        animatorStateSamples(controller, runtime, states[static_cast<std::size_t>(lr.fade_state)], lr.fade_phase,
                             fade_dphase, 1.0f - fresh, lookup, &out, scratch);
    }
    current_begin = out.size();
    animatorStateSamples(controller, runtime, st, lr.phase, dphase, fresh, lookup, &out, scratch);
}

// --- Montages ------------------------------------------------------------------------

void startMontage(AnimatorMontage& m) {
    m.active = true;
    m.just_started = true;
    m.clip_index = -1;
    m.time = 0.0f;
    m.delta = 0.0f;
    m.weight = 0.0f;
    m.elapsed = 0.0f;
    m.stopping = false;
    m.stop_elapsed = 0.0f;
    m.jump_section.clear();
    m.section.clear();
    ++m.serial;
}

int clipSectionAt(const asset::AnimationClip& clip, float time) {
    int found = -1;
    for (std::size_t i = 0; i < clip.sections.size(); ++i) {
        if (clip.sections[i].time <= time + 1e-5f) found = static_cast<int>(i);
    }
    return found;
}

namespace {
int sectionByName(const asset::AnimationClip& clip, const std::string& name) {
    for (std::size_t i = 0; i < clip.sections.size(); ++i) {
        if (clip.sections[i].name == name) return static_cast<int>(i);
    }
    return -1;
}
// Donde acaba la seccion i (empieza la siguiente o el final del clip).
float sectionEnd(const asset::AnimationClip& clip, int i) {
    if (i >= 0 && static_cast<std::size_t>(i) + 1 < clip.sections.size()) return clip.sections[static_cast<std::size_t>(i) + 1].time;
    return clip.duration;
}
}  // namespace

void stepMontage(AnimatorMontage& m, const asset::AnimationClip& clip, float dt, std::vector<std::uint32_t>* events) {
    if (!m.active) return;
    const float duration = std::max(clip.duration, 0.0f);
    m.delta = 0.0f;
    if (m.just_started) {
        m.just_started = false;
        const int start = m.start_section.empty() ? -1 : sectionByName(clip, m.start_section);
        m.time = start >= 0 ? clip.sections[static_cast<std::size_t>(start)].time : 0.0f;
        m.elapsed = 0.0f;
        if (events != nullptr) anim::collectClipEvents(clip, m.time, 0.0f, false, true, *events);
    } else {
        m.elapsed += dt;
        if (!m.jump_section.empty()) {
            // Salto pedido por el juego: lo que hay entre medias no suena.
            const int target = sectionByName(clip, m.jump_section);
            m.jump_section.clear();
            if (target >= 0) {
                m.time = clip.sections[static_cast<std::size_t>(target)].time;
                if (events != nullptr) anim::collectClipEvents(clip, m.time, 0.0f, false, true, *events);
            }
        } else {
            float remaining = dt * std::max(m.speed, 0.0f);
            // Secciones: al llegar al final de una con `next`, salta (bucle si es ella misma).
            for (int guard = 0; remaining > 0.0f && guard < 16; ++guard) {
                const int index = clipSectionAt(clip, m.time);
                const float end = sectionEnd(clip, index);
                const int next = index >= 0 && !clip.sections[static_cast<std::size_t>(index)].next.empty()
                                     ? sectionByName(clip, clip.sections[static_cast<std::size_t>(index)].next)
                                     : -1;
                if (next < 0 || m.time + remaining < end) {
                    const float to = std::min(m.time + remaining, duration);
                    if (events != nullptr) anim::collectClipEvents(clip, to, to - m.time, false, false, *events);
                    m.delta += to - m.time;
                    m.time = to;
                    break;
                }
                // Hasta el final de la seccion y salto al principio de `next`.
                if (events != nullptr) anim::collectClipEvents(clip, end, end - m.time, false, false, *events);
                m.delta += end - m.time;
                remaining -= end - m.time;
                const float target = clip.sections[static_cast<std::size_t>(next)].time;
                if (end - target <= 1e-4f && end - m.time <= 1e-6f) break;  // seccion vacia: no da vueltas sin fin
                m.time = target;
                if (events != nullptr) anim::collectClipEvents(clip, m.time, 0.0f, false, true, *events);
            }
        }
    }
    const int index = clipSectionAt(clip, m.time);
    m.section = index >= 0 ? clip.sections[static_cast<std::size_t>(index)].name : std::string{};

    // Peso: entrada, salida (al final del clip, si no salta a otra seccion) o stop.
    float in = m.blend_in > 0.0f ? smooth(m.elapsed / m.blend_in) : 1.0f;
    float out = 1.0f;
    bool done = false;
    if (m.stopping) {
        m.stop_elapsed += dt;
        out = m.stop_blend > 0.0f ? 1.0f - smooth(m.stop_elapsed / m.stop_blend) : 0.0f;
        done = m.stop_elapsed >= m.stop_blend;
    } else {
        const bool jumps = index >= 0 && !clip.sections[static_cast<std::size_t>(index)].next.empty() &&
                           sectionByName(clip, clip.sections[static_cast<std::size_t>(index)].next) >= 0;
        if (!jumps) {
            const float left = duration - m.time;
            if (m.blend_out > 0.0f && left < m.blend_out) out = smooth(left / m.blend_out);
            done = m.time >= duration - 1e-5f;
        }
    }
    m.weight = std::min(in, out);
    if (done) {
        m.active = false;
        m.weight = 0.0f;
    }
}

// --- ClipMeta ----------------------------------------------------------------------------

void ClipMeta::sort() {
    std::stable_sort(events.begin(), events.end(),
                     [](const asset::ClipEvent& a, const asset::ClipEvent& b) { return a.time < b.time; });
    for (asset::ClipCurve& c : curves) {
        std::stable_sort(c.keys.begin(), c.keys.end(),
                         [](const asset::CurveKey& a, const asset::CurveKey& b) { return a.time < b.time; });
    }
    std::stable_sort(sections.begin(), sections.end(),
                     [](const asset::ClipSection& a, const asset::ClipSection& b) { return a.time < b.time; });
}

ClipMeta clipMetaOf(const asset::AnimationClip& clip) {
    ClipMeta m;
    m.events = clip.events;
    m.curves = clip.curves;
    m.sections = clip.sections;
    return m;
}

void applyClipMeta(asset::AnimationClip& clip, ClipMeta meta) {
    meta.sort();
    clip.events = std::move(meta.events);
    clip.curves = std::move(meta.curves);
    clip.sections = std::move(meta.sections);
}

namespace {
json clipMetaToJson(const ClipMeta& meta) {
    json j;
    j["version"] = kClipMetaVersion;
    json events = json::array();
    for (const asset::ClipEvent& e : meta.events) {
        json ev = {{"name", e.name}, {"time", e.time}};
        if (!e.params.empty()) ev["params"] = e.params;
        events.push_back(std::move(ev));
    }
    j["events"] = std::move(events);
    json curves = json::array();
    for (const asset::ClipCurve& c : meta.curves) {
        json keys = json::array();
        for (const asset::CurveKey& k : c.keys) keys.push_back({k.time, k.value});
        curves.push_back({{"name", c.name}, {"keys", std::move(keys)}});
    }
    j["curves"] = std::move(curves);
    json sections = json::array();
    for (const asset::ClipSection& s : meta.sections) {
        json sec = {{"name", s.name}, {"time", s.time}};
        if (!s.next.empty()) sec["next"] = s.next;
        sections.push_back(std::move(sec));
    }
    j["sections"] = std::move(sections);
    return j;
}

ClipMeta clipMetaFromJson(const json& j) {
    ClipMeta meta;
    if (!j.is_object()) return meta;
    // Version mas nueva que la nuestra: se lee lo que se entienda.
    if (const auto it = j.find("events"); it != j.end() && it->is_array()) {
        for (const json& e : *it) {
            if (!e.is_object()) continue;
            asset::ClipEvent ev;
            ev.name = e.value("name", std::string{});
            ev.time = e.value("time", 0.0f);
            if (const auto p = e.find("params"); p != e.end()) ev.params = p->is_string() ? p->get<std::string>() : p->dump();
            if (!ev.name.empty() && std::isfinite(ev.time)) meta.events.push_back(std::move(ev));
        }
    }
    if (const auto it = j.find("curves"); it != j.end() && it->is_array()) {
        for (const json& c : *it) {
            if (!c.is_object()) continue;
            asset::ClipCurve curve;
            curve.name = c.value("name", std::string{});
            if (const auto k = c.find("keys"); k != c.end() && k->is_array()) {
                for (const json& key : *k) {
                    if (key.is_array() && key.size() == 2 && key[0].is_number() && key[1].is_number()) {
                        curve.keys.push_back({key[0].get<float>(), key[1].get<float>()});
                    }
                }
            }
            if (!curve.name.empty()) meta.curves.push_back(std::move(curve));
        }
    }
    if (const auto it = j.find("sections"); it != j.end() && it->is_array()) {
        for (const json& s : *it) {
            if (!s.is_object()) continue;
            asset::ClipSection sec;
            sec.name = s.value("name", std::string{});
            sec.time = s.value("time", 0.0f);
            sec.next = s.value("next", std::string{});
            if (!sec.name.empty()) meta.sections.push_back(std::move(sec));
        }
    }
    meta.sort();
    return meta;
}
}  // namespace

bool loadClipMetaFromCranim(const std::filesystem::path& path, ClipMeta& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + path.string();
        return false;
    }
    std::string first;
    std::getline(file, first);
    const json header = json::parse(first, nullptr, false);
    if (header.is_discarded() || !header.is_object()) {
        if (error) *error = "cabecera danada: " + path.string();
        return false;
    }
    const auto it = header.find("meta");
    out = it != header.end() ? clipMetaFromJson(*it) : ClipMeta{};
    return true;
}

bool saveClipMetaToCranim(const std::filesystem::path& path, const ClipMeta& meta, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + path.string();
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    const std::size_t cut = text.find('\n');
    json header = json::parse(text.substr(0, cut), nullptr, false);
    if (header.is_discarded() || !header.is_object()) {
        if (error) *error = "cabecera danada: " + path.string();
        return false;
    }
    if (meta.empty()) {
        header.erase("meta");
    } else {
        header["meta"] = clipMetaToJson(meta);
    }
    // La cabecera sigue en UNA linea (la base de datos lee solo la primera).
    const std::string rest = cut == std::string::npos ? std::string("\n") : text.substr(cut);
    return writeText(path, header.dump() + rest, error);
}

std::filesystem::path clipMetaSidecarPath(const std::filesystem::path& model_path) {
    std::filesystem::path p = model_path;
    p += ".crclipmeta";
    return p;
}

bool loadModelClipMeta(const std::filesystem::path& sidecar, std::map<std::string, ClipMeta>& out, std::string* error) {
    out.clear();
    std::ifstream file(sidecar, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + sidecar.string();
        return false;
    }
    const json root = json::parse(file, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        if (error) *error = "JSON danado en " + sidecar.string();
        return false;
    }
    if (const auto it = root.find("clips"); it != root.end() && it->is_object()) {
        for (auto c = it->begin(); c != it->end(); ++c) out[c.key()] = clipMetaFromJson(c.value());
    }
    return true;
}

bool saveModelClipMeta(const std::filesystem::path& sidecar, const std::map<std::string, ClipMeta>& clips,
                       std::string* error) {
    json root;
    root["type"] = "clip_meta";
    root["version"] = kClipMetaVersion;
    json& list = root["clips"] = json::object();
    // Tambien los vacios: al recargar, un clip sin eventos los pierde.
    for (const auto& [name, meta] : clips) list[name] = clipMetaToJson(meta);
    return writeText(sidecar, root.dump(2), error);
}

int addClipEvent(ClipMeta& meta, const std::string& name, float time, float duration) {
    asset::ClipEvent ev;
    ev.name = name.empty() ? std::string("Evento") : name;
    ev.time = std::clamp(time, 0.0f, std::max(duration, 0.0f));
    const auto at = std::upper_bound(meta.events.begin(), meta.events.end(), ev.time,
                                     [](float t, const asset::ClipEvent& e) { return t < e.time; });
    const auto it = meta.events.insert(at, std::move(ev));
    return static_cast<int>(it - meta.events.begin());
}

int moveClipEvent(ClipMeta& meta, int index, float time, float duration) {
    if (index < 0 || index >= static_cast<int>(meta.events.size())) return -1;
    asset::ClipEvent ev = std::move(meta.events[static_cast<std::size_t>(index)]);
    meta.events.erase(meta.events.begin() + index);
    ev.time = std::clamp(time, 0.0f, std::max(duration, 0.0f));
    // Tras los de su mismo tiempo: arrastrar no salta por encima de otro igual.
    const auto at = std::upper_bound(meta.events.begin(), meta.events.end(), ev.time,
                                     [](float t, const asset::ClipEvent& e) { return t < e.time; });
    const auto it = meta.events.insert(at, std::move(ev));
    return static_cast<int>(it - meta.events.begin());
}

bool removeClipEvent(ClipMeta& meta, int index) {
    if (index < 0 || index >= static_cast<int>(meta.events.size())) return false;
    meta.events.erase(meta.events.begin() + index);
    return true;
}

void collectSampleEvents(const asset::ModelData& model, const anim::ClipSample* samples, std::size_t count,
                         bool include_start, int layer, AnimatorRuntime& runtime) {
    // Solo el clip que mas pesa (en un Blend Tree, un paso por pie, no uno por clip).
    const anim::ClipSample* best = nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        const anim::ClipSample& s = samples[i];
        if (s.clip < 0 || static_cast<std::size_t>(s.clip) >= model.animations.size() || !(s.weight > 1e-4f)) continue;
        if (best == nullptr || s.weight > best->weight) best = &s;
    }
    if (best == nullptr) return;
    const asset::AnimationClip& clip = model.animations[static_cast<std::size_t>(best->clip)];
    if (clip.events.empty()) return;
    thread_local std::vector<std::uint32_t> fired;
    fired.clear();
    anim::collectClipEvents(clip, best->time, best->delta, best->loop, include_start, fired);
    for (const std::uint32_t k : fired) {
        const asset::ClipEvent& e = clip.events[k];
        runtime.pushEvent(AnimatorFiredEvent{e.name, clip.name, e.params, e.time, layer});
    }
}

void accumulateSampleCurves(const asset::ModelData& model, const anim::ClipSample* samples, std::size_t count,
                            float weight, std::unordered_map<std::string, float>& curves) {
    float total = 0.0f;
    for (std::size_t i = 0; i < count; ++i) total += std::max(samples[i].weight, 0.0f);
    if (total <= 1e-6f || weight <= 0.0f) return;
    // Por curva: la media de los clips que la tienen; con `weight` sustituye
    // a lo que hubiera debajo (como una capa Override).
    thread_local std::unordered_map<std::string, std::pair<float, float>> sums;
    sums.clear();
    for (std::size_t i = 0; i < count; ++i) {
        const anim::ClipSample& s = samples[i];
        if (s.clip < 0 || static_cast<std::size_t>(s.clip) >= model.animations.size() || !(s.weight > 0.0f)) continue;
        for (const asset::ClipCurve& c : model.animations[static_cast<std::size_t>(s.clip)].curves) {
            auto& [value, w] = sums[c.name];
            value += c.sample(s.time) * s.weight;
            w += s.weight;
        }
    }
    for (const auto& [name, vw] : sums) {
        if (vw.second <= 1e-6f) continue;
        const float v = vw.first / vw.second;
        const auto it = curves.find(name);
        curves[name] = it == curves.end() ? v * std::min(weight, 1.0f) : it->second + (v - it->second) * std::min(weight, 1.0f);
    }
}

float timelineTimeAt(float x, float x0, float width, float duration, float fps) {
    if (width <= 0.0f || duration <= 0.0f) return 0.0f;
    float t = std::clamp((x - x0) / width, 0.0f, 1.0f) * duration;
    if (fps > 0.0f) t = std::clamp(std::round(t * fps) / fps, 0.0f, duration);
    return t;
}

float timelineX(float time, float x0, float width, float duration) {
    if (duration <= 0.0f) return x0;
    return x0 + std::clamp(time / duration, 0.0f, 1.0f) * width;
}

void ClipPreviewPlayer::step(float dt, float duration) {
    if (!playing || duration <= 0.0f) return;
    time += dt * speed;
    if (loop) {
        time -= std::floor(time / duration) * duration;
    } else if (time >= duration || time <= 0.0f) {
        time = std::clamp(time, 0.0f, duration);
        playing = false;
    }
}

json clipMetaToJsonPublic(const ClipMeta& meta) { return clipMetaToJson(meta); }
ClipMeta clipMetaFromJsonPublic(const json& j) { return clipMetaFromJson(j); }

}  // namespace cramion::ecs
