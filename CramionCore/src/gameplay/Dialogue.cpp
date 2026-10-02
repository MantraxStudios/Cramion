#include "CramionCore/gameplay/Dialogue.h"
#include "CramionCore/ecs/World.h"  // la definicion de ComponentRegistry::registerComponent<T>

#include "CramionCore/gameplay/Localization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace cramion::gameplay {

using ecs::FloatRange;
using ecs::Vec3Kind;
using json = nlohmann::json;

namespace {

constexpr std::array<const char*, kDialogueNodeTypeCount> kTypeKeys = {"start", "line", "choice", "condition",
                                                                       "set", "event", "jump", "end"};
constexpr std::array<const char*, kDialogueNodeTypeCount> kTypeNames = {"Inicio", "Linea", "Opciones", "Condicion",
                                                                        "Variable", "Evento", "Saltar", "Fin"};

DialogueNodeType typeFromKey(const std::string& key) {
    for (int i = 0; i < kDialogueNodeTypeCount; ++i) {
        if (key == kTypeKeys[static_cast<std::size_t>(i)]) return static_cast<DialogueNodeType>(i);
    }
    return DialogueNodeType::Line;
}

json conditionJson(const DialogueCondition& c) { return json{{"variable", c.variable}, {"op", c.op}, {"value", c.value}}; }

DialogueCondition conditionFrom(const json& j) {
    DialogueCondition c;
    if (!j.is_object()) return c;
    c.variable = j.value("variable", std::string());
    c.op = j.value("op", std::string("=="));
    if (const auto it = j.find("value"); it != j.end()) c.value = it->is_string() ? it->get<std::string>() : it->dump();
    return c;
}

// Texto de un valor ("true", "3", "hola") como lo escribe el usuario a un JSON.
json parseValue(const std::string& text) {
    const std::string t = text;
    if (t == "true") return true;
    if (t == "false") return false;
    if (!t.empty()) {
        char* end = nullptr;
        const double d = std::strtod(t.c_str(), &end);
        if (end != nullptr && *end == '\0') return d;
    }
    // Entre comillas: texto literal.
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
    return t;
}

std::string valueText(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number()) {
        const double d = v.get<double>();
        if (std::floor(d) == d && std::fabs(d) < 1e15) {
            std::ostringstream ss;
            ss << static_cast<long long>(d);
            return ss.str();
        }
        std::ostringstream ss;
        ss << d;
        return ss.str();
    }
    if (v.is_null()) return "";
    return v.dump();
}

bool truthy(const json& v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_string()) return !v.get<std::string>().empty();
    return false;
}

}  // namespace

const char* dialogueNodeTypeName(DialogueNodeType type) {
    const int i = static_cast<int>(type);
    return i >= 0 && i < kDialogueNodeTypeCount ? kTypeNames[static_cast<std::size_t>(i)] : "?";
}

const char* dialogueNodeTypeKey(DialogueNodeType type) {
    const int i = static_cast<int>(type);
    return i >= 0 && i < kDialogueNodeTypeCount ? kTypeKeys[static_cast<std::size_t>(i)] : "line";
}

// --- Asset ---

DialogueNode* DialogueAsset::find(int id) {
    for (DialogueNode& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const DialogueNode* DialogueAsset::find(int id) const {
    for (const DialogueNode& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const DialogueNode* DialogueAsset::start() const {
    for (const DialogueNode& n : nodes) {
        if (n.type == DialogueNodeType::Start) return &n;
    }
    return nodes.empty() ? nullptr : &nodes.front();
}

DialogueNode& DialogueAsset::add(DialogueNodeType type, core::Vec2 position) {
    for (const DialogueNode& n : nodes) next_id = std::max(next_id, n.id + 1);
    DialogueNode node;
    node.id = next_id++;
    node.type = type;
    node.position = position;
    if (type == DialogueNodeType::Line) {
        node.speaker = "Personaje";
        node.text = "Nuevo texto";
    } else if (type == DialogueNodeType::Choice) {
        node.options.push_back(DialogueOption{"Opcion 1", "", {}, true, -1});
        node.options.push_back(DialogueOption{"Opcion 2", "", {}, true, -1});
    } else if (type == DialogueNodeType::Condition) {
        node.conditions.push_back(DialogueCondition{"variable", "==", "true"});
    } else if (type == DialogueNodeType::SetVariable) {
        node.variable = "variable";
        node.value = "true";
    } else if (type == DialogueNodeType::Event) {
        node.event = "evento";
    }
    nodes.push_back(std::move(node));
    return nodes.back();
}

void DialogueAsset::remove(int id) {
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const DialogueNode& n) { return n.id == id; }), nodes.end());
    for (DialogueNode& n : nodes) {
        if (n.next == id) n.next = -1;
        if (n.next_false == id) n.next_false = -1;
        if (n.jump_node == id) n.jump_node = -1;
        for (DialogueOption& o : n.options) {
            if (o.next == id) o.next = -1;
        }
    }
}

bool parseDialogue(const std::string& text, DialogueAsset& out, std::string* error) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = "el .crdialog no es JSON valido";
        return false;
    }
    out = DialogueAsset{};
    out.uuid = Uuid::parse(j.value("uuid", std::string()));
    out.name = j.value("name", std::string());
    if (const auto it = j.find("variables"); it != j.end() && it->is_array()) {
        for (const json& v : *it) {
            if (!v.is_object()) continue;
            DialogueVariable var;
            var.name = v.value("name", std::string());
            var.value = v.contains("value") ? v["value"] : json(false);
            if (!var.name.empty()) out.variables.push_back(std::move(var));
        }
    }
    if (const auto it = j.find("nodes"); it != j.end() && it->is_array()) {
        for (const json& n : *it) {
            if (!n.is_object()) continue;
            DialogueNode node;
            node.id = n.value("id", 0);
            node.type = typeFromKey(n.value("type", std::string("line")));
            if (const auto p = n.find("position"); p != n.end() && p->is_array() && p->size() >= 2) {
                node.position = core::Vec2{(*p)[0].get<float>(), (*p)[1].get<float>()};
            }
            node.comment = n.value("comment", std::string());
            node.speaker = n.value("speaker", std::string());
            node.text = n.value("text", std::string());
            node.text_key = n.value("text_key", std::string());
            node.audio = n.value("audio", std::string());
            node.auto_advance = n.value("auto_advance", 0.0f);
            if (const auto o = n.find("options"); o != n.end() && o->is_array()) {
                for (const json& oj : *o) {
                    if (!oj.is_object()) continue;
                    DialogueOption option;
                    option.text = oj.value("text", std::string());
                    option.text_key = oj.value("text_key", std::string());
                    if (oj.contains("condition")) option.condition = conditionFrom(oj["condition"]);
                    option.hide_if_false = oj.value("hide_if_false", true);
                    option.next = oj.value("next", -1);
                    node.options.push_back(std::move(option));
                }
            }
            if (const auto c = n.find("conditions"); c != n.end() && c->is_array()) {
                for (const json& cj : *c) node.conditions.push_back(conditionFrom(cj));
            }
            node.require_all = n.value("require_all", true);
            node.next_false = n.value("next_false", -1);
            node.variable = n.value("variable", std::string());
            node.op = n.value("op", std::string("="));
            if (const auto v = n.find("value"); v != n.end()) node.value = v->is_string() ? v->get<std::string>() : v->dump();
            node.event = n.value("event", std::string());
            node.argument = n.value("argument", std::string());
            node.target = n.value("target", std::string());
            node.method = n.value("method", std::string());
            node.jump_node = n.value("jump_node", -1);
            node.jump_dialogue = n.value("jump_dialogue", std::string());
            node.next = n.value("next", -1);
            out.next_id = std::max(out.next_id, node.id + 1);
            out.nodes.push_back(std::move(node));
        }
    }
    // Ids repetidos o 0 (escrito a mano): se renumeran.
    for (std::size_t i = 0; i < out.nodes.size(); ++i) {
        bool repeated = out.nodes[i].id <= 0;
        for (std::size_t k = 0; k < i && !repeated; ++k) repeated = out.nodes[k].id == out.nodes[i].id;
        if (repeated) out.nodes[i].id = out.next_id++;
    }
    out.next_id = std::max(out.next_id, j.value("next_id", 1));
    return true;
}

std::string serializeDialogue(const DialogueAsset& asset) {
    json j;
    j["format"] = "CramionDialogue";
    j["version"] = 1;
    j["uuid"] = asset.uuid.valid() ? asset.uuid.toString() : Uuid::generate().toString();
    j["name"] = asset.name;
    j["next_id"] = asset.next_id;
    j["variables"] = json::array();
    for (const DialogueVariable& v : asset.variables) j["variables"].push_back({{"name", v.name}, {"value", v.value}});
    j["nodes"] = json::array();
    for (const DialogueNode& n : asset.nodes) {
        json node;
        node["id"] = n.id;
        node["type"] = dialogueNodeTypeKey(n.type);
        node["position"] = {n.position.x, n.position.y};
        if (!n.comment.empty()) node["comment"] = n.comment;
        switch (n.type) {
            case DialogueNodeType::Line:
                node["speaker"] = n.speaker;
                node["text"] = n.text;
                if (!n.text_key.empty()) node["text_key"] = n.text_key;
                if (!n.audio.empty()) node["audio"] = n.audio;
                if (n.auto_advance > 0.0f) node["auto_advance"] = n.auto_advance;
                break;
            case DialogueNodeType::Choice: {
                json options = json::array();
                for (const DialogueOption& o : n.options) {
                    json oj{{"text", o.text}, {"next", o.next}};
                    if (!o.text_key.empty()) oj["text_key"] = o.text_key;
                    if (!o.condition.variable.empty()) oj["condition"] = conditionJson(o.condition);
                    if (!o.hide_if_false) oj["hide_if_false"] = false;
                    options.push_back(std::move(oj));
                }
                node["options"] = std::move(options);
                break;
            }
            case DialogueNodeType::Condition: {
                json conditions = json::array();
                for (const DialogueCondition& c : n.conditions) conditions.push_back(conditionJson(c));
                node["conditions"] = std::move(conditions);
                node["require_all"] = n.require_all;
                node["next_false"] = n.next_false;
                break;
            }
            case DialogueNodeType::SetVariable:
                node["variable"] = n.variable;
                node["op"] = n.op;
                node["value"] = n.value;
                break;
            case DialogueNodeType::Event:
                node["event"] = n.event;
                if (!n.argument.empty()) node["argument"] = n.argument;
                if (!n.target.empty()) node["target"] = n.target;
                if (!n.method.empty()) node["method"] = n.method;
                break;
            case DialogueNodeType::Jump:
                node["jump_node"] = n.jump_node;
                if (!n.jump_dialogue.empty()) node["jump_dialogue"] = n.jump_dialogue;
                break;
            default: break;
        }
        if (n.type != DialogueNodeType::Choice && n.type != DialogueNodeType::Jump && n.type != DialogueNodeType::End) {
            node["next"] = n.next;
        }
        j["nodes"].push_back(std::move(node));
    }
    return j.dump(2, ' ', false, json::error_handler_t::replace);
}

bool loadDialogue(const std::filesystem::path& file, DialogueAsset& out, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "no se pudo abrir " + file.filename().string();
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!parseDialogue(ss.str(), out, error)) return false;
    if (out.name.empty()) {
        const std::u8string stem = file.stem().u8string();
        out.name = std::string(stem.begin(), stem.end());
    }
    return true;
}

bool saveDialogue(const std::filesystem::path& file, const DialogueAsset& asset, std::string* error) {
    std::error_code e;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), e);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "no se pudo escribir " + file.filename().string();
        return false;
    }
    out << serializeDialogue(asset);
    return static_cast<bool>(out);
}

DialogueAsset makeExampleDialogue(const std::string& name) {
    DialogueAsset a;
    a.uuid = Uuid::generate();
    a.name = name;
    a.variables.push_back(DialogueVariable{"oro", 10});
    a.variables.push_back(DialogueVariable{"conoce_al_mercader", false});

    DialogueNode& start = a.add(DialogueNodeType::Start, {40.0f, 160.0f});
    const int start_id = start.id;
    DialogueNode& cond = a.add(DialogueNodeType::Condition, {220.0f, 160.0f});
    cond.conditions = {DialogueCondition{"conoce_al_mercader", "==", "true"}};
    const int cond_id = cond.id;
    DialogueNode& hello = a.add(DialogueNodeType::Line, {480.0f, 60.0f});
    hello.speaker = "Mercader";
    hello.text = "¡Bienvenido, viajero! Nunca te habia visto por aqui.";
    const int hello_id = hello.id;
    DialogueNode& again = a.add(DialogueNodeType::Line, {480.0f, 260.0f});
    again.speaker = "Mercader";
    again.text = "¡Otra vez tu! Tienes {$oro} monedas, ¿que te llevas?";
    const int again_id = again.id;
    DialogueNode& met = a.add(DialogueNodeType::SetVariable, {740.0f, 60.0f});
    met.variable = "conoce_al_mercader";
    met.op = "=";
    met.value = "true";
    const int met_id = met.id;
    DialogueNode& choice = a.add(DialogueNodeType::Choice, {980.0f, 160.0f});
    choice.options.clear();
    const int choice_id = choice.id;
    DialogueNode& buy = a.add(DialogueNodeType::SetVariable, {1240.0f, 40.0f});
    buy.variable = "oro";
    buy.op = "-=";
    buy.value = "5";
    const int buy_id = buy.id;
    DialogueNode& event = a.add(DialogueNodeType::Event, {1480.0f, 40.0f});
    event.event = "comprar_pocion";
    event.argument = "pocion";
    const int event_id = event.id;
    DialogueNode& thanks = a.add(DialogueNodeType::Line, {1720.0f, 40.0f});
    thanks.speaker = "Mercader";
    thanks.text = "¡Gracias! Te quedan {$oro} monedas.";
    const int thanks_id = thanks.id;
    DialogueNode& bye = a.add(DialogueNodeType::Line, {1240.0f, 280.0f});
    bye.speaker = "Mercader";
    bye.text = "¡Buen viaje!";
    const int bye_id = bye.id;
    DialogueNode& end = a.add(DialogueNodeType::End, {1980.0f, 160.0f});
    const int end_id = end.id;

    a.find(start_id)->next = cond_id;
    a.find(cond_id)->next = again_id;
    a.find(cond_id)->next_false = hello_id;
    a.find(hello_id)->next = met_id;
    a.find(met_id)->next = choice_id;
    a.find(again_id)->next = choice_id;
    DialogueNode* c = a.find(choice_id);
    c->options.push_back(DialogueOption{"Comprar una pocion (5 de oro)", "", DialogueCondition{"oro", ">=", "5"}, false, buy_id});
    c->options.push_back(DialogueOption{"Adios", "", {}, true, bye_id});
    a.find(buy_id)->next = event_id;
    a.find(event_id)->next = thanks_id;
    a.find(thanks_id)->next = end_id;
    a.find(bye_id)->next = end_id;
    return a;
}

// --- Ejecucion ---

json DialogueSystem::variable(const std::string& name) const {
    const auto it = variables_.find(name);
    return it != variables_.end() ? *it : json(nullptr);
}

bool DialogueSystem::evaluate(const DialogueCondition& c) const {
    if (c.variable.empty()) return true;
    const json left = variable(c.variable);
    json right = !c.value.empty() && c.value[0] == '$' ? variable(c.value.substr(1)) : parseValue(c.value);
    const std::string& op = c.op;
    if (op == "true" || op == "es verdad") return truthy(left);
    if (op == "false" || op == "es falso") return !truthy(left);
    // Numeros (tambien un texto que es un numero).
    const auto number = [](const json& v, double& out) {
        if (v.is_number()) {
            out = v.get<double>();
            return true;
        }
        if (v.is_boolean()) {
            out = v.get<bool>() ? 1.0 : 0.0;
            return true;
        }
        if (v.is_string()) {
            const std::string s = v.get<std::string>();
            char* end = nullptr;
            out = std::strtod(s.c_str(), &end);
            return !s.empty() && end != nullptr && *end == '\0';
        }
        if (v.is_null()) {
            out = 0.0;
            return true;
        }
        return false;
    };
    double a = 0.0;
    double b = 0.0;
    if (number(left, a) && number(right, b)) {
        if (op == "==") return a == b;
        if (op == "!=") return a != b;
        if (op == "<") return a < b;
        if (op == "<=") return a <= b;
        if (op == ">") return a > b;
        if (op == ">=") return a >= b;
        return false;
    }
    const std::string sa = valueText(left);
    const std::string sb = valueText(right);
    if (op == "==") return sa == sb;
    if (op == "!=") return sa != sb;
    if (op == "<") return sa < sb;
    if (op == "<=") return sa <= sb;
    if (op == ">") return sa > sb;
    if (op == ">=") return sa >= sb;
    return false;
}

std::string DialogueSystem::interpolate(const std::string& text) const {
    if (text.find("{$") == std::string::npos) return text;
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '$') {
            const std::size_t close = text.find('}', i + 2);
            if (close != std::string::npos) {
                out += valueText(variable(text.substr(i + 2, close - i - 2)));
                i = close;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

std::string DialogueSystem::optionText(const DialogueOption& option) const {
    const std::string base = option.text_key.empty() ? option.text : localization().get(option.text_key);
    return interpolate(base);
}

bool DialogueSystem::start(const std::string& dialogue, std::string* error) {
    std::filesystem::path file = resolver_ ? resolver_(dialogue) : std::filesystem::path{};
    if (file.empty()) file = std::filesystem::path(std::u8string(dialogue.begin(), dialogue.end()));
    DialogueAsset asset;
    if (!loadDialogue(file, asset, error)) return false;
    return start(asset);
}

bool DialogueSystem::start(const DialogueAsset& asset, int from_node) {
    if (active_) stop();
    asset_ = asset;
    // Variables declaradas que aun no existen: su valor inicial.
    for (const DialogueVariable& v : asset_.variables) {
        if (!variables_.contains(v.name)) variables_[v.name] = v.value;
    }
    const DialogueNode* first = from_node >= 0 ? asset_.find(from_node) : asset_.start();
    if (first == nullptr) return false;
    active_ = true;
    line_ = DialogueLine{};
    choices_.clear();
    choice_targets_.clear();
    DialogueEvent ev;
    ev.kind = DialogueEvent::Kind::Started;
    ev.dialogue = asset_.name;
    events_.push_back(std::move(ev));
    run(first->type == DialogueNodeType::Start ? first->next : first->id);
    return true;
}

void DialogueSystem::run(int node_id) {
    // Los nodos que no esperan se ejecutan seguidos; un tope evita bucles
    // infinitos (Saltar a si mismo).
    for (int guard = 0; guard < 10000; ++guard) {
        const DialogueNode* node = asset_.find(node_id);
        if (node == nullptr) {
            emitEnd();
            return;
        }
        switch (node->type) {
            case DialogueNodeType::Start: node_id = node->next; continue;
            case DialogueNodeType::End: emitEnd(); return;
            case DialogueNodeType::Line: {
                line_ = DialogueLine{};
                line_.node = node->id;
                line_.speaker = interpolate(node->speaker);
                line_.text = interpolate(node->text_key.empty() ? node->text : localization().get(node->text_key));
                line_.audio = node->audio;
                line_.auto_advance = node->auto_advance;
                line_time_ = 0.0f;
                revealed_ = false;
                choices_.clear();
                choice_targets_.clear();
                pending_next_ = node->next;
                ++revision_;
                DialogueEvent ev;
                ev.kind = DialogueEvent::Kind::Line;
                ev.dialogue = asset_.name;
                ev.line = line_;
                events_.push_back(std::move(ev));
                // Linea seguida de Opciones: se ensenan ya, con la linea encima.
                if (const DialogueNode* next = asset_.find(node->next); next != nullptr && next->type == DialogueNodeType::Choice) {
                    node_id = next->id;
                    continue;
                }
                return;
            }
            case DialogueNodeType::Choice: {
                choices_.clear();
                choice_targets_.clear();
                for (const DialogueOption& o : node->options) {
                    const bool ok = evaluate(o.condition);
                    if (!ok && o.hide_if_false) continue;
                    DialogueChoice c;
                    c.index = static_cast<int>(choices_.size());
                    c.text = optionText(o);
                    c.enabled = ok;
                    choices_.push_back(std::move(c));
                    choice_targets_.push_back(o.next);
                }
                if (choices_.empty()) {  // ninguna disponible: se termina
                    emitEnd();
                    return;
                }
                pending_next_ = -1;
                ++revision_;
                DialogueEvent ev;
                ev.kind = DialogueEvent::Kind::Choices;
                ev.dialogue = asset_.name;
                ev.line = line_;
                ev.choices = choices_;
                events_.push_back(std::move(ev));
                return;
            }
            case DialogueNodeType::Condition: {
                bool result = node->require_all;
                for (const DialogueCondition& c : node->conditions) {
                    const bool r = evaluate(c);
                    if (node->require_all) {
                        result = result && r;
                    } else {
                        result = result || r;
                    }
                }
                if (node->conditions.empty()) result = true;
                node_id = result ? node->next : node->next_false;
                continue;
            }
            case DialogueNodeType::SetVariable: {
                if (!node->variable.empty()) {
                    const json value = !node->value.empty() && node->value[0] == '$' ? variable(node->value.substr(1))
                                                                                    : parseValue(node->value);
                    json& target = variables_[node->variable];
                    if (node->op == "+=" || node->op == "-=") {
                        const double current = target.is_number() ? target.get<double>() : 0.0;
                        const double delta = value.is_number() ? value.get<double>() : 0.0;
                        const double result = node->op == "+=" ? current + delta : current - delta;
                        target = result;
                    } else if (node->op == "toggle") {
                        target = !truthy(target);
                    } else {
                        target = value;
                    }
                }
                node_id = node->next;
                continue;
            }
            case DialogueNodeType::Event: {
                DialogueEvent ev;
                ev.kind = DialogueEvent::Kind::Event;
                ev.dialogue = asset_.name;
                ev.name = node->event;
                ev.argument = interpolate(node->argument);
                ev.target = node->target;
                ev.method = node->method;
                events_.push_back(std::move(ev));
                node_id = node->next;
                continue;
            }
            case DialogueNodeType::Jump: {
                if (!node->jump_dialogue.empty()) {
                    // Copias: `node` apunta dentro de asset_, que se va a sustituir.
                    const std::string other_name = node->jump_dialogue;
                    const int other_node = node->jump_node;
                    std::filesystem::path file = resolver_ ? resolver_(other_name) : std::filesystem::path{};
                    if (file.empty()) file = std::filesystem::path(std::u8string(other_name.begin(), other_name.end()));
                    DialogueAsset other;
                    if (!loadDialogue(file, other)) {
                        emitEnd();
                        return;
                    }
                    asset_ = std::move(other);
                    for (const DialogueVariable& v : asset_.variables) {
                        if (!variables_.contains(v.name)) variables_[v.name] = v.value;
                    }
                    // Un nodo concreto del otro dialogo, o su Inicio.
                    const DialogueNode* s = other_node >= 0 ? asset_.find(other_node) : nullptr;
                    if (s == nullptr) s = asset_.start();
                    if (s == nullptr) {
                        emitEnd();
                        return;
                    }
                    node_id = s->type == DialogueNodeType::Start ? s->next : s->id;
                    continue;
                }
                node_id = node->jump_node;
                continue;
            }
        }
    }
    emitEnd();
}

void DialogueSystem::advance() {
    if (!active_ || !choices_.empty()) return;
    run(pending_next_);
}

bool DialogueSystem::choose(int index) {
    if (!active_ || index < 0 || index >= static_cast<int>(choices_.size())) return false;
    if (!choices_[static_cast<std::size_t>(index)].enabled) return false;
    const int target = choice_targets_[static_cast<std::size_t>(index)];
    choices_.clear();
    choice_targets_.clear();
    run(target);
    return true;
}

void DialogueSystem::emitEnd() {
    if (!active_) return;
    active_ = false;
    line_ = DialogueLine{};
    choices_.clear();
    choice_targets_.clear();
    pending_next_ = -1;
    ++revision_;
    DialogueEvent ev;
    ev.kind = DialogueEvent::Kind::Ended;
    ev.dialogue = asset_.name;
    events_.push_back(std::move(ev));
}

void DialogueSystem::stop() { emitEnd(); }

void DialogueSystem::update(float dt) {
    if (!active_) return;
    line_time_ += dt;
    if (line_.node >= 0 && choices_.empty() && line_.auto_advance > 0.0f && line_time_ >= line_.auto_advance) advance();
}

std::vector<DialogueEvent> DialogueSystem::takeEvents() {
    std::vector<DialogueEvent> out;
    out.swap(events_);
    return out;
}

namespace {
DialogueSystem* g_active = nullptr;
}

DialogueSystem* activeDialogue() { return g_active; }
void setActiveDialogue(DialogueSystem* system) { g_active = system; }

// --- Caja de dialogo ---

void DialogueBox::reflect(ecs::PropertyVisitor& v) {
    if (v.beginGroup("Panel")) {
        v.field({"panel_color", "Color"}, panel_color, Vec3Kind::Color);
        v.field({"panel_alpha", "Opacidad"}, panel_alpha, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"corner_radius", "Esquinas"}, corner_radius, FloatRange{0.0f, 100.0f, 0.5f, "%.1f"});
        v.endGroup();
    }
    if (v.beginGroup("Texto")) {
        v.field({"speaker_color", "Color del nombre"}, speaker_color, Vec3Kind::Color);
        v.field({"text_color", "Color del texto"}, text_color, Vec3Kind::Color);
        v.field({"font_size", "Tamano del texto"}, font_size, FloatRange{8.0f, 120.0f, 0.5f, "%.0f"});
        v.field({"speaker_size", "Tamano del nombre"}, speaker_size, FloatRange{8.0f, 120.0f, 0.5f, "%.0f"});
        v.field({"chars_per_second", "Letras por segundo", "Efecto maquina de escribir (0 = todo de golpe)"},
                chars_per_second, FloatRange{0.0f, 400.0f, 1.0f, "%.0f"});
        v.endGroup();
    }
    if (v.beginGroup("Opciones")) {
        v.field({"choice_color", "Color"}, choice_color, Vec3Kind::Color);
        v.field({"choice_hover", "Color al pasar"}, choice_hover, Vec3Kind::Color);
        v.endGroup();
    }
    v.field({"click_to_continue", "Clic para seguir", "Clic, Espacio o Enter pasan a la siguiente linea"}, click_to_continue);
    v.field({"show_hint", "Indicador de seguir"}, show_hint);
}

void registerDialogueComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("DialogueBox") != nullptr) return;
    registry.registerComponent<DialogueBox>("DialogueBox", "Caja de dialogo", "UI");
}

}  // namespace cramion::gameplay
