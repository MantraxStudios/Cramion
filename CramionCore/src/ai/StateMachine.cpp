#include "CramionCore/ai/StateMachine.h"

// La definicion de ComponentRegistry::registerComponent<T>.
#include "CramionCore/ecs/World.h"
#include "CramionCore/ecs/Reflection.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace cramion::ai {

using nlohmann::json;

namespace {

constexpr const char* kVarKeys[] = {"bool", "int", "float", "string", "entity", "vec3"};
constexpr const char* kVarLabels[] = {"Bool", "Int", "Float", "String", "Entity", "Vec3"};
constexpr const char* kCompareKeys[] = {"==", "!=", ">", "<", ">=", "<=", "true", "false"};
constexpr const char* kCompareLabels[] = {"igual a", "distinto de", "mayor que", "menor que",
                                          "mayor o igual que", "menor o igual que", "es verdadero", "es falso"};
constexpr const char* kConditionKeys[] = {"variable", "trigger", "timer", "expression"};
constexpr std::size_t kHistory = 12;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string formatNumber(double n) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", n);
    return buffer;
}

json vec2(const core::Vec2& v) { return json::array({v.x, v.y}); }
json vec3(const core::Vec3& v) { return json::array({v.x, v.y, v.z}); }

core::Vec2 readVec2(const json& j, core::Vec2 fallback) {
    if (j.is_array() && j.size() == 2 && j[0].is_number() && j[1].is_number()) {
        return core::Vec2{j[0].get<float>(), j[1].get<float>()};
    }
    return fallback;
}

core::Vec3 readVec3(const json& j, core::Vec3 fallback) {
    if (j.is_array() && j.size() == 3 && j[0].is_number() && j[1].is_number() && j[2].is_number()) {
        return core::Vec3{j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    }
    return fallback;
}

// Un valor del JSON (numero, bool, texto, [x,y,z]) como texto para Value::parse.
std::string jsonValueText(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_boolean()) return j.get<bool>() ? "true" : "false";
    if (j.is_number_integer()) return std::to_string(j.get<long long>());
    if (j.is_number()) return formatNumber(j.get<double>());
    if (j.is_array()) {
        std::string out;
        for (const json& x : j) {
            if (!out.empty()) out += ' ';
            out += x.is_number() ? formatNumber(x.get<double>()) : std::string("0");
        }
        return out;
    }
    return {};
}

// El valor como JSON nativo (asi el archivo se lee bien).
json valueJson(const Value& v) {
    switch (v.type) {
        case VarType::Bool: return v.b;
        case VarType::Int: return static_cast<long long>(std::llround(v.n));
        case VarType::Float: return v.n;
        case VarType::Vector: return vec3(v.v);
        default: return v.s;
    }
}

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

// --- Pizarra -------------------------------------------------------------------

const char* varTypeKey(VarType type) { return kVarKeys[std::clamp(static_cast<int>(type), 0, kVarTypeCount - 1)]; }
const char* varTypeLabel(VarType type) { return kVarLabels[std::clamp(static_cast<int>(type), 0, kVarTypeCount - 1)]; }

VarType varTypeFromKey(const std::string& key, VarType fallback) {
    const std::string k = lower(key);
    for (int i = 0; i < kVarTypeCount; ++i) {
        if (k == kVarKeys[i]) return static_cast<VarType>(i);
    }
    if (k == "number" || k == "double" || k == "real") return VarType::Float;
    if (k == "integer") return VarType::Int;
    if (k == "boolean") return VarType::Bool;
    if (k == "text" || k == "str") return VarType::String;
    if (k == "vector" || k == "vector3") return VarType::Vector;
    if (k == "object" || k == "gameobject") return VarType::Entity;
    return fallback;
}

Value Value::parse(VarType type, const std::string& text) {
    Value v;
    v.type = type;
    switch (type) {
        case VarType::Bool: {
            const std::string t = lower(text);
            v.b = t == "true" || t == "1" || t == "si" || t == "yes" || t == "on";
            break;
        }
        case VarType::Int: v.n = static_cast<double>(std::llround(std::strtod(text.c_str(), nullptr))); break;
        case VarType::Float: v.n = std::strtod(text.c_str(), nullptr); break;
        case VarType::String:
        case VarType::Entity: v.s = text; break;
        case VarType::Vector: {
            std::string t = text;
            std::replace(t.begin(), t.end(), ',', ' ');
            std::istringstream ss(t);
            ss >> v.v.x >> v.v.y >> v.v.z;
            break;
        }
    }
    return v;
}

std::string Value::text() const {
    switch (type) {
        case VarType::Bool: return b ? "true" : "false";
        case VarType::Int: return std::to_string(std::llround(n));
        case VarType::Float: return formatNumber(n);
        case VarType::Vector: return formatNumber(v.x) + " " + formatNumber(v.y) + " " + formatNumber(v.z);
        default: return s;
    }
}

double Value::number() const {
    switch (type) {
        case VarType::Bool: return b ? 1.0 : 0.0;
        case VarType::Int:
        case VarType::Float: return n;
        case VarType::Vector: return std::sqrt(static_cast<double>(v.x * v.x + v.y * v.y + v.z * v.z));
        default: return 0.0;
    }
}

bool Value::truthy() const {
    switch (type) {
        case VarType::Bool: return b;
        case VarType::Int:
        case VarType::Float: return n != 0.0;
        case VarType::String: return !s.empty();
        case VarType::Entity: return entity != kNoEntity;
        case VarType::Vector: return v.x != 0.0f || v.y != 0.0f || v.z != 0.0f;
    }
    return false;
}

const char* compareKey(Compare c) { return kCompareKeys[std::clamp(static_cast<int>(c), 0, kCompareCount - 1)]; }
const char* compareLabel(Compare c) { return kCompareLabels[std::clamp(static_cast<int>(c), 0, kCompareCount - 1)]; }

Compare compareFromKey(const std::string& key, Compare fallback) {
    const std::string k = lower(key);
    for (int i = 0; i < kCompareCount; ++i) {
        if (k == kCompareKeys[i]) return static_cast<Compare>(i);
    }
    if (k == "=" || k == "eq" || k == "equal" || k == "equals") return Compare::Equal;
    if (k == "~=" || k == "ne" || k == "not_equal" || k == "notequal") return Compare::NotEqual;
    if (k == "gt" || k == "greater") return Compare::Greater;
    if (k == "lt" || k == "less") return Compare::Less;
    if (k == "ge" || k == "gte") return Compare::GreaterEqual;
    if (k == "le" || k == "lte") return Compare::LessEqual;
    if (k == "if" || k == "is_true" || k == "istrue") return Compare::IsTrue;
    if (k == "if_not" || k == "is_false" || k == "isfalse" || k == "not") return Compare::IsFalse;
    return fallback;
}

// --- Asset ---------------------------------------------------------------------

int StateMachineAsset::findState(const std::string& name) const {
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (states[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

const Variable* StateMachineAsset::findVariable(const std::string& name) const {
    for (const Variable& v : variables) {
        if (v.name == name) return &v;
    }
    return nullptr;
}

Variable* StateMachineAsset::findVariable(const std::string& name) {
    for (Variable& v : variables) {
        if (v.name == name) return &v;
    }
    return nullptr;
}

void StateMachineAsset::removeState(int index) {
    if (index < 0 || index >= static_cast<int>(states.size())) return;
    states.erase(states.begin() + index);
    std::vector<Transition> kept;
    for (Transition t : transitions) {
        if (t.from == index || t.to == index) continue;
        if (t.from > index) --t.from;
        if (t.to > index) --t.to;
        kept.push_back(std::move(t));
    }
    transitions = std::move(kept);
    if (entry_state == index) entry_state = 0;
    else if (entry_state > index) --entry_state;
    entry_state = std::clamp(entry_state, 0, std::max(0, static_cast<int>(states.size()) - 1));
}

void StateMachineAsset::renameVariable(const std::string& from, const std::string& to) {
    if (Variable* v = findVariable(from)) v->name = to;
    for (Transition& t : transitions) {
        for (Condition& c : t.conditions) {
            if (c.kind == ConditionKind::Variable && c.variable == from) c.variable = to;
            if (c.kind == ConditionKind::Variable && c.value == "$" + from) c.value = "$" + to;
        }
    }
}

std::string stateMachineToJson(const StateMachineAsset& m) {
    json root;
    root["uuid"] = m.uuid.valid() ? m.uuid.toString() : Uuid::generate().toString();
    root["type"] = "state_machine";
    root["version"] = 1;
    // Por nombre si los nombres no se repiten (se lee mejor); si no, indice.
    std::set<std::string> names;
    bool unique = true;
    for (const State& s : m.states) unique = names.insert(s.name).second && unique;
    const auto stateRef = [&](int index) -> json {
        if (index == kAnyState) return "any";
        if (unique && index >= 0 && index < static_cast<int>(m.states.size())) return m.states[index].name;
        return index;
    };
    root["entry"] = stateRef(std::clamp(m.entry_state, 0, std::max(0, static_cast<int>(m.states.size()) - 1)));
    root["entry_position"] = vec2(m.entry_position);
    root["any_state_position"] = vec2(m.any_state_position);
    if (!m.any_code.empty()) root["any_code"] = m.any_code;
    json& variables = root["variables"] = json::array();
    for (const Variable& v : m.variables) {
        variables.push_back({{"name", v.name}, {"type", varTypeKey(v.value.type)}, {"value", valueJson(v.value)}});
    }
    json& states = root["states"] = json::array();
    for (const State& s : m.states) {
        json state = {{"name", s.name}, {"position", vec2(s.position)}, {"color", vec3(s.color)}, {"send_update", s.send_update}};
        if (!s.code.empty()) state["code"] = s.code;
        if (!s.script.empty()) state["script"] = s.script;
        states.push_back(std::move(state));
    }
    json& transitions = root["transitions"] = json::array();
    for (const Transition& t : m.transitions) {
        json conditions = json::array();
        for (const Condition& c : t.conditions) {
            json k = {{"type", kConditionKeys[static_cast<int>(c.kind)]}};
            switch (c.kind) {
                case ConditionKind::Variable:
                    k["variable"] = c.variable;
                    k["compare"] = compareKey(c.compare);
                    if (c.compare != Compare::IsTrue && c.compare != Compare::IsFalse) k["value"] = c.value;
                    break;
                case ConditionKind::Trigger: k["trigger"] = c.variable; break;
                case ConditionKind::Timer: k["seconds"] = c.seconds; break;
                case ConditionKind::Expression: k["expression"] = c.expression; break;
            }
            conditions.push_back(std::move(k));
        }
        json tj = {{"from", stateRef(t.from)}, {"to", stateRef(t.to)}, {"conditions", std::move(conditions)}};
        if (t.priority != 0) tj["priority"] = t.priority;
        if (t.allow_self) tj["allow_self"] = true;
        transitions.push_back(std::move(tj));
    }
    return root.dump(2);
}

bool stateMachineFromJson(const std::string& text, StateMachineAsset& out, std::string* error) {
    const json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        if (error) *error = "JSON danado";
        return false;
    }
    StateMachineAsset m;
    m.uuid = Uuid::parse(root.value("uuid", std::string{}));
    if (const auto it = root.find("entry_position"); it != root.end()) m.entry_position = readVec2(*it, m.entry_position);
    if (const auto it = root.find("any_state_position"); it != root.end()) {
        m.any_state_position = readVec2(*it, m.any_state_position);
    }
    if (const auto it = root.find("any_code"); it != root.end() && it->is_string()) m.any_code = it->get<std::string>();
    if (const auto it = root.find("variables"); it != root.end()) {
        // Lista [{name, type, value}] u objeto {nombre: valor} (el tipo sale del valor).
        if (it->is_array()) {
            for (const json& j : *it) {
                if (!j.is_object()) continue;
                Variable v;
                v.name = j.value("name", std::string{});
                if (v.name.empty()) continue;
                const json value = j.contains("value") ? j["value"] : json(nullptr);
                VarType type = VarType::Float;
                if (value.is_boolean()) type = VarType::Bool;
                else if (value.is_number_integer()) type = VarType::Int;
                else if (value.is_string()) type = VarType::String;
                else if (value.is_array()) type = VarType::Vector;
                type = varTypeFromKey(j.value("type", std::string{}), type);
                v.value = Value::parse(type, jsonValueText(value));
                m.variables.push_back(std::move(v));
            }
        } else if (it->is_object()) {
            for (const auto& [name, value] : it->items()) {
                VarType type = VarType::Float;
                if (value.is_boolean()) type = VarType::Bool;
                else if (value.is_string()) type = VarType::String;
                else if (value.is_array()) type = VarType::Vector;
                m.variables.push_back(Variable{name, Value::parse(type, jsonValueText(value))});
            }
        }
    }
    if (const auto it = root.find("states"); it != root.end() && it->is_array()) {
        int index = 0;
        for (const json& j : *it) {
            if (!j.is_object()) continue;
            State s;
            s.name = j.value("name", std::string{"Estado"});
            s.position = core::Vec2{static_cast<float>(index % 3) * 230.0f, static_cast<float>(index / 3) * 120.0f};
            if (const auto p = j.find("position"); p != j.end()) s.position = readVec2(*p, s.position);
            if (const auto c = j.find("color"); c != j.end()) s.color = readVec3(*c, s.color);
            s.code = j.value("code", std::string{});
            s.script = j.value("script", std::string{});
            // Los de antes con codigo Lua: su script de C++ necesita OnStateUpdate.
            s.send_update = j.value("send_update", hasLuaCode(s.code) || !s.script.empty());
            m.states.push_back(std::move(s));
            ++index;
        }
    }
    const int count = static_cast<int>(m.states.size());
    // "Patrullar", 2, "any" -> indice (o -2 si no vale).
    const auto stateIndex = [&](const json& j) -> int {
        if (j.is_number_integer()) {
            const int i = j.get<int>();
            return i == kAnyState || (i >= 0 && i < count) ? i : -2;
        }
        if (j.is_string()) {
            const std::string name = j.get<std::string>();
            const std::string l = lower(name);
            if (l == "any" || l == "any state" || l == "cualquier estado" || l == "cualquiera") return kAnyState;
            const int found = m.findState(name);
            return found >= 0 ? found : -2;
        }
        return -2;
    };
    if (const auto it = root.find("entry"); it != root.end()) {
        m.entry_state = std::max(0, stateIndex(*it));
    } else {
        m.entry_state = root.value("entry_state", 0);
    }
    if (const auto it = root.find("transitions"); it != root.end() && it->is_array()) {
        for (const json& j : *it) {
            if (!j.is_object()) continue;
            Transition t;
            t.from = j.contains("from") ? stateIndex(j["from"]) : -2;
            t.to = j.contains("to") ? stateIndex(j["to"]) : -2;
            if (t.from == -2 || t.to < 0) {
                if (error && error->empty()) *error = "transicion con un estado que no existe: " + j.dump();
                continue;
            }
            t.priority = j.value("priority", 0);
            t.allow_self = j.value("allow_self", false);
            if (const auto k = j.find("conditions"); k != j.end() && k->is_array()) {
                for (const json& jc : *k) {
                    if (!jc.is_object()) continue;
                    Condition c;
                    const std::string kind = lower(jc.value("type", std::string{"variable"}));
                    c.kind = ConditionKind::Variable;
                    for (int i = 0; i < kConditionKindCount; ++i) {
                        if (kind == kConditionKeys[i]) c.kind = static_cast<ConditionKind>(i);
                    }
                    if (kind == "event" || kind == "trigger") c.kind = ConditionKind::Trigger;
                    if (kind == "time" || kind == "after" || kind == "wait") c.kind = ConditionKind::Timer;
                    if (kind == "lua" || kind == "expression" || kind == "expr" || kind == "code") {
                        c.kind = ConditionKind::Expression;
                    }
                    switch (c.kind) {
                        case ConditionKind::Variable:
                            c.variable = jc.value("variable", std::string{});
                            c.compare = compareFromKey(jc.value("compare", std::string{"=="}), Compare::Equal);
                            if (jc.contains("value")) c.value = jsonValueText(jc["value"]);
                            break;
                        case ConditionKind::Trigger:
                            c.variable = jc.value("trigger", jc.value("variable", std::string{}));
                            break;
                        case ConditionKind::Timer: c.seconds = std::max(0.0f, jc.value("seconds", 1.0f)); break;
                        case ConditionKind::Expression: c.expression = jc.value("expression", std::string{"true"}); break;
                    }
                    t.conditions.push_back(std::move(c));
                }
            }
            m.transitions.push_back(std::move(t));
        }
    }
    m.entry_state = std::clamp(m.entry_state, 0, std::max(0, count - 1));
    out = std::move(m);
    return true;
}

bool loadStateMachine(const std::filesystem::path& path, StateMachineAsset& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + path.string();
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    if (!stateMachineFromJson(ss.str(), out, nullptr)) {
        if (error) *error = "JSON danado en " + path.string();
        return false;
    }
    return true;
}

bool saveStateMachine(const StateMachineAsset& machine, const std::filesystem::path& path, std::string* error) {
    return writeText(path, stateMachineToJson(machine), error);
}

std::vector<std::string> validateStateMachine(const StateMachineAsset& m) {
    std::vector<std::string> problems;
    const int count = static_cast<int>(m.states.size());
    if (count == 0) problems.push_back("No hay estados");
    std::set<std::string> names;
    for (const State& s : m.states) {
        if (s.name.empty()) problems.push_back("Un estado no tiene nombre");
        else if (!names.insert(s.name).second) problems.push_back("Hay dos estados \"" + s.name + "\" (sm:go no sabra cual)");
    }
    for (std::size_t i = 0; i < m.transitions.size(); ++i) {
        const Transition& t = m.transitions[i];
        const std::string where = "Transicion " + std::to_string(i + 1);
        if (t.to < 0 || t.to >= count || t.from < kAnyState || t.from >= count) {
            problems.push_back(where + ": estado que no existe");
            continue;
        }
        for (const Condition& c : t.conditions) {
            if (c.kind == ConditionKind::Variable) {
                if (m.findVariable(c.variable) == nullptr) {
                    problems.push_back(where + ": no existe la variable \"" + c.variable + "\"");
                }
                if (c.value.size() > 1 && c.value[0] == '$' && m.findVariable(c.value.substr(1)) == nullptr) {
                    problems.push_back(where + ": no existe la variable \"" + c.value.substr(1) + "\"");
                }
            } else if (c.kind == ConditionKind::Trigger && c.variable.empty()) {
                problems.push_back(where + ": trigger sin nombre");
            } else if (c.kind == ConditionKind::Expression) {
                const Expression& e = c.compiled.get(c.expression);
                if (!e.valid()) {
                    problems.push_back(where + ": la expresion \"" + c.expression + "\" no se puede evaluar (" + e.error() +
                                       "); la condicion sera falsa");
                }
            }
        }
    }
    // Codigo Lua de antes: ya no corre (un aviso para toda la maquina).
    std::string with_code;
    for (const State& s : m.states) {
        if (!hasLuaCode(s.code) && s.script.empty()) continue;
        with_code += (with_code.empty() ? "" : ", ") + s.name;
    }
    if (hasLuaCode(m.any_code)) with_code += (with_code.empty() ? "" : ", ") + std::string("Cualquier estado");
    if (!with_code.empty()) {
        problems.push_back("Codigo Lua en " + with_code +
                           ": ya no se ejecuta; convierte el codigo del estado a un script de C++ (OnStateEnter / "
                           "OnStateUpdate / OnStateExit en onMessage)");
    }
    return problems;
}

bool hasLuaCode(const std::string& code) {
    // Cada linea sin comentarios: vacia, "end" o "function Nombre(...)" no cuentan.
    std::istringstream in(code);
    std::string line;
    bool block = false;  // --[[ ... ]]
    while (std::getline(in, line)) {
        if (block) {
            const std::size_t close = line.find("]]");
            if (close == std::string::npos) continue;
            line = line.substr(close + 2);
            block = false;
        }
        if (const std::size_t dash = line.find("--"); dash != std::string::npos) {
            block = line.compare(dash, 4, "--[[") == 0 && line.find("]]", dash + 4) == std::string::npos;
            line.erase(dash);
        }
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
        if (line == "end") continue;
        if (line.rfind("function ", 0) == 0 && line.back() == ')') continue;
        return true;
    }
    return false;
}

// --- Ejecucion -----------------------------------------------------------------

Value* Runtime::find(const std::string& name) {
    for (Variable& v : vars) {
        if (v.name == name) return &v.value;
    }
    return nullptr;
}

const Value* Runtime::find(const std::string& name) const {
    for (const Variable& v : vars) {
        if (v.name == name) return &v.value;
    }
    return nullptr;
}

void resetRuntime(const StateMachineAsset& machine, Runtime& runtime, const std::vector<VariableOverride>& overrides) {
    const bool running = runtime.running;
    runtime = Runtime{};
    runtime.running = running;
    runtime.vars = machine.variables;
    for (const VariableOverride& o : overrides) {
        if (Value* v = runtime.find(o.name)) *v = Value::parse(v->type, o.value);
    }
}

bool conditionHolds(const StateMachineAsset& /*machine*/, const Runtime& runtime, const Condition& c) {
    switch (c.kind) {
        case ConditionKind::Trigger:
            return std::find(runtime.triggers.begin(), runtime.triggers.end(), c.variable) != runtime.triggers.end();
        case ConditionKind::Timer: return runtime.state_time >= c.seconds;
        case ConditionKind::Expression: return c.compiled.get(c.expression).test(runtime.vars);
        case ConditionKind::Variable: break;
    }
    const Value* left = runtime.find(c.variable);
    if (left == nullptr) return false;
    if (c.compare == Compare::IsTrue) return left->truthy();
    if (c.compare == Compare::IsFalse) return !left->truthy();
    // Lo de la derecha: otra variable ("$nombre") o un valor con el tipo de la izquierda.
    Value right;
    if (c.value.size() > 1 && c.value[0] == '$') {
        const Value* other = runtime.find(c.value.substr(1));
        if (other == nullptr) return false;
        right = *other;
    } else {
        right = Value::parse(left->type, c.value);
    }
    int order = 0;  // <0, 0, >0
    switch (left->type) {
        case VarType::String: {
            const std::string r = right.type == VarType::String || right.type == VarType::Entity ? right.s : right.text();
            order = left->s.compare(r);
            break;
        }
        case VarType::Entity: {
            if (right.type == VarType::Entity && (right.entity != kNoEntity || left->entity != kNoEntity) &&
                c.value.size() > 1 && c.value[0] == '$') {
                order = left->entity == right.entity ? 0 : 1;
            } else {
                order = left->s.compare(right.s);
            }
            if (c.compare != Compare::Equal && c.compare != Compare::NotEqual) return false;
            break;
        }
        case VarType::Vector: {
            if (c.compare == Compare::Equal || c.compare == Compare::NotEqual) {
                const core::Vec3 r = right.type == VarType::Vector ? right.v : core::Vec3{};
                const bool same = std::abs(left->v.x - r.x) < 1e-4f && std::abs(left->v.y - r.y) < 1e-4f &&
                                  std::abs(left->v.z - r.z) < 1e-4f;
                return (c.compare == Compare::Equal) == same;
            }
            // Mayor/menor: la longitud del vector.
            const double a = left->number();
            const double b = right.type == VarType::Vector ? right.number() : std::strtod(c.value.c_str(), nullptr);
            order = a < b ? -1 : (a > b ? 1 : 0);
            break;
        }
        default: {
            const double a = left->number();
            const double b = right.number();
            if (left->type == VarType::Float && (c.compare == Compare::Equal || c.compare == Compare::NotEqual)) {
                order = std::abs(a - b) < 1e-6 ? 0 : (a < b ? -1 : 1);
            } else {
                order = a < b ? -1 : (a > b ? 1 : 0);
            }
            break;
        }
    }
    switch (c.compare) {
        case Compare::Equal: return order == 0;
        case Compare::NotEqual: return order != 0;
        case Compare::Greater: return order > 0;
        case Compare::Less: return order < 0;
        case Compare::GreaterEqual: return order >= 0;
        case Compare::LessEqual: return order <= 0;
        default: return false;
    }
}

int pickTransition(const StateMachineAsset& machine, const Runtime& runtime) {
    const int count = static_cast<int>(machine.states.size());
    // Candidatas en orden: prioridad mayor, luego Cualquier estado, luego la lista.
    std::vector<int> order;
    for (std::size_t i = 0; i < machine.transitions.size(); ++i) {
        const Transition& t = machine.transitions[i];
        if (t.to < 0 || t.to >= count) continue;
        if (t.from == kAnyState) {
            if (t.to == runtime.state && !t.allow_self) continue;
        } else if (t.from != runtime.state) {
            continue;
        }
        order.push_back(static_cast<int>(i));
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Transition& ta = machine.transitions[a];
        const Transition& tb = machine.transitions[b];
        if (ta.priority != tb.priority) return ta.priority > tb.priority;
        return (ta.from == kAnyState) > (tb.from == kAnyState);
    });
    for (const int i : order) {
        const Transition& t = machine.transitions[i];
        bool ok = true;
        for (std::size_t k = 0; k < t.conditions.size() && ok; ++k) {
            ok = conditionHolds(machine, runtime, t.conditions[k]);
        }
        if (ok) return i;
    }
    return -1;
}

void changeState(const StateMachineAsset& machine, Runtime& runtime, int to, int transition) {
    const int count = static_cast<int>(machine.states.size());
    if (to < 0 || to >= count) return;
    const std::string from_name =
        runtime.state >= 0 && runtime.state < count ? machine.states[runtime.state].name : std::string("Entrada");
    runtime.previous = runtime.state;
    runtime.state = to;
    runtime.state_time = 0.0f;
    runtime.last_transition = transition;
    runtime.last_change_time = runtime.time;
    ++runtime.changes;
    char when[32];
    std::snprintf(when, sizeof(when), "%.2f s", static_cast<double>(runtime.time));
    runtime.history.push_back(std::string(when) + "  " + from_name + " -> " + machine.states[to].name +
                              (transition == -2 ? "  (codigo)" : ""));
    if (runtime.history.size() > kHistory) runtime.history.erase(runtime.history.begin());
}

StepResult startStateMachine(const StateMachineAsset& machine, Runtime& runtime) {
    StepResult r;
    if (machine.states.empty()) return r;
    runtime.started = true;
    runtime.state = -1;
    const int entry = std::clamp(machine.entry_state, 0, static_cast<int>(machine.states.size()) - 1);
    changeState(machine, runtime, entry, -1);
    r.changed = true;
    r.to = entry;
    return r;
}

StepResult applyRequestedState(const StateMachineAsset& machine, Runtime& runtime) {
    StepResult r;
    const int to = runtime.requested;
    runtime.requested = -1;
    if (to < 0 || to >= static_cast<int>(machine.states.size())) return r;
    r.changed = true;
    r.from = runtime.state;
    r.to = to;
    r.transition = -2;
    changeState(machine, runtime, to, -2);
    return r;
}

StepResult stepStateMachine(const StateMachineAsset& machine, Runtime& runtime, float dt) {
    StepResult r;
    if (!runtime.started || !runtime.running || machine.states.empty()) return r;
    runtime.time += dt;
    runtime.state_time += dt;
    if (runtime.requested >= 0) {
        r = applyRequestedState(machine, runtime);
    } else {
        const int t = pickTransition(machine, runtime);
        if (t >= 0) {
            r.changed = true;
            r.from = runtime.state;
            r.to = machine.transitions[t].to;
            r.transition = t;
            changeState(machine, runtime, r.to, t);
        }
    }
    // Los triggers valen hasta que se miran una vez.
    runtime.triggers.clear();
    return r;
}

// --- Ejemplo -------------------------------------------------------------------

StateMachineAsset exampleEnemyStateMachine() {
    StateMachineAsset m;
    const auto var = [&](const char* name, VarType type, const char* value) {
        m.variables.push_back(Variable{name, Value::parse(type, value)});
    };
    var("objetivo", VarType::Entity, "Jugador");
    var("distancia", VarType::Float, "9999");
    var("rangoVision", VarType::Float, "10");
    var("rangoAtaque", VarType::Float, "2");
    var("rangoPerder", VarType::Float, "16");
    var("vida", VarType::Float, "100");
    var("danio", VarType::Float, "10");
    var("radioPatrulla", VarType::Float, "8");
    var("casa", VarType::Vector, "0 0 0");

    // Cada estado lo hace el script de C++ del enemigo (onMessage): moverse
    // en OnStateEnter, perseguir y golpear en OnStateUpdate, pararse en
    // OnStateExit. El script tambien calcula `distancia` cada frame (sm.set).
    const auto state = [](const char* name, core::Vec2 position, core::Vec3 color, bool update) {
        State s;
        s.name = name;
        s.position = position;
        s.color = color;
        s.send_update = update;
        return s;
    };
    const State patrol = state("Patrullar", {0.0f, 0.0f}, {0.20f, 0.45f, 0.25f}, true);
    const State chase = state("Perseguir", {260.0f, 0.0f}, {0.62f, 0.45f, 0.12f}, true);
    const State attack = state("Atacar", {520.0f, 0.0f}, {0.62f, 0.18f, 0.18f}, true);
    const State flee = state("Huir", {260.0f, 200.0f}, {0.45f, 0.25f, 0.60f}, true);
    const State back = state("Volver", {0.0f, 200.0f}, {0.20f, 0.38f, 0.60f}, true);

    m.states = {patrol, chase, attack, flee, back};
    m.entry_state = 0;
    const auto compare = [](const char* variable, Compare c, const char* value) {
        Condition k;
        k.kind = ConditionKind::Variable;
        k.variable = variable;
        k.compare = c;
        k.value = value;
        return k;
    };
    Condition heard;
    heard.kind = ConditionKind::Trigger;
    heard.variable = "ruido";
    Condition settle;
    settle.kind = ConditionKind::Timer;
    settle.seconds = 0.5f;
    Condition healed;
    healed.kind = ConditionKind::Expression;
    healed.expression = "vida >= 60";

    m.transitions.push_back(Transition{0, 1, 0, false, {compare("distancia", Compare::Less, "$rangoVision")}});
    m.transitions.push_back(Transition{0, 1, 0, false, {heard}});
    m.transitions.push_back(Transition{1, 2, 0, false, {compare("distancia", Compare::LessEqual, "$rangoAtaque")}});
    m.transitions.push_back(Transition{2, 1, 0, false, {compare("distancia", Compare::Greater, "$rangoAtaque"), settle}});
    m.transitions.push_back(Transition{1, 4, 0, false, {compare("distancia", Compare::Greater, "$rangoPerder")}});
    m.transitions.push_back(Transition{kAnyState, 3, 10, false, {compare("vida", Compare::Less, "30")}});
    m.transitions.push_back(Transition{3, 4, 0, false, {healed}});
    m.transitions.push_back(Transition{4, 1, 0, false, {compare("distancia", Compare::Less, "$rangoVision")}});
    return m;
}

// --- Componente ------------------------------------------------------------------

void StateMachine::reflect(ecs::PropertyVisitor& v) {
    v.asset({"machine", "Maquina de estados", "Asset .crfsm: estados y transiciones (la logica, en un script de C++)"}, machine,
            assets::AssetType::StateMachine);
    v.field({"start_active", "Empieza activa", "Si no, espera a sm:start() desde un script"}, start_active);
    v.field({"debug", "Depurar", "Cada cambio de estado sale en la Consola"}, debug);
    // El Inspector los dibuja aparte (con el control del tipo de cada variable).
    if (v.wantsAllFields()) {
        ecs::listField(v, {"variables", "Variables"}, variables, [](VariableOverride& o, ecs::PropertyVisitor& item) {
            item.field({"name", "Nombre"}, o.name);
            item.field({"type", "Tipo"}, o.type, 0, kVarTypeCount - 1);
            o.type = std::clamp(o.type, 0, kVarTypeCount - 1);
            item.field({"value", "Valor"}, o.value);
        });
    }
}

void registerStateMachineComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("StateMachine") == nullptr) {
        registry.registerComponent<StateMachine>("StateMachine", "Maquina de estados (IA)", "Scripting");
    }
}

}  // namespace cramion::ai
