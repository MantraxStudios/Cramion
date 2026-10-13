// Visual Scripts (.crgraph): el interprete de los grafos.
//
// Cada .crgraph se lee una vez (y se vuelve a leer al guardarlo en Play:
// rt.onReload); compileGraph lo valida y un grafo con errores no se ejecuta.
// Cada objeto con el componente VisualScript tiene su instancia: sus
// variables (las del grafo, con lo del Inspector), el ultimo valor de cada
// pin, el estado de los nodos que lo tienen (Do Once, Gate...), las esperas
// y los temporizadores.
//
// Ejecucion:
//   - Los pines exec llevan el orden: un nodo hace lo suyo y sigue por su
//     salida. Los nodos puros (sin exec) se calculan cuando alguien usa su
//     valor, cada vez (como en Unreal).
//   - Las llamadas a la API (nodos "call", "call.get", "call.set" y los
//     integrados que la usan) van por rt.native.call / get / set, asi que
//     llegan a todo lo que registran los modulos.
//   - Delay no para el resto del grafo: la cadena se corta y se guarda por
//     donde iba (el Delay y los Sequence / For / For Each / While que lo
//     contienen, de dentro afuera); en otro frame sigue desde ahi.
// Fases: Start y Update en Phase::Update (antes, las esperas y los
// temporizadores), LateUpdate en Phase::Late, FixedUpdate en el paso fijo,
// los choques en Phase::Events, Destroy al destruir el objeto o parar Play.
// Los Custom Event los lanzan Call Event, Send Event, Set Timer, la interfaz
// (ScriptSystem::callMethod) y Send Event de otros grafos.
// Un error (una funcion de la API que falla...) sale en la Consola con el
// nodo y para esa instancia hasta que se recargue el grafo.

#include "Modules.h"

#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/scripting/VisualScript.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cramion::scripting::native {

namespace {

using vscript::Graph;
using vscript::Literal;
using vscript::Node;
using vscript::PinType;

using PinKey = std::pair<int, int>;  // (nodo, pin)

std::string trimText(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool numberFromText(const std::string& text, double& out) {
    const std::string t = trimText(text);
    if (t.empty()) return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end != nullptr && *end == '\0';
}

// --- El grafo leido ------------------------------------------------------------------

struct Loaded {
    // Lo que el interprete pregunta en cada uso (calculado una vez al leer).
    struct Meta {
        bool pure = false;
        std::vector<PinType> inputs;  // tipo real de cada entrada
    };

    std::string file;
    Graph graph;
    std::unordered_map<int, const Node*> nodes;
    std::unordered_map<int, Meta> meta;
    std::map<PinKey, PinKey> input_links;   // entrada -> (nodo, salida) de donde viene
    std::map<PinKey, PinKey> output_links;  // salida exec -> (nodo, entrada) a donde va
    std::map<PinKey, Literal> literals;     // valor escrito en cada entrada
    std::map<std::string, std::vector<const Node*>> events;  // por tipo ("event.update")
    std::map<std::string, const Node*> custom;               // Custom Event por nombre
    std::set<int> breakpoints;  // los guardados en el archivo

    const Node* node(int id) const {
        const auto it = nodes.find(id);
        return it == nodes.end() ? nullptr : it->second;
    }
    const std::vector<const Node*>& eventsOf(const std::string& kind) const {
        static const std::vector<const Node*> kNone;
        const auto it = events.find(kind);
        return it == events.end() ? kNone : it->second;
    }
};

bool reserved(const std::string& name) {
    static const std::set<std::string> kReserved = {
        "Awake", "Start", "Update", "LateUpdate", "FixedUpdate", "OnDestroy", "OnCollisionEnter", "OnCollisionStay",
        "OnCollisionExit", "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit", "OnOriginShift", "OnNetVar", "properties", "entity"};
    return kReserved.contains(name) || name.rfind("__", 0) == 0;
}

std::shared_ptr<Loaded> prepare(const std::string& file, Graph graph) {
    auto out = std::make_shared<Loaded>();
    out->file = file;
    out->graph = std::move(graph);
    const Graph& g = out->graph;
    for (const Node& n : g.nodes) {
        out->nodes[n.id] = &n;
        Loaded::Meta& meta = out->meta[n.id];
        meta.pure = vscript::isPureNode(n);
        meta.inputs.resize(n.inputs.size(), PinType::Any);
        for (std::size_t i = 0; i < n.inputs.size(); ++i) {
            const PinType t = vscript::nodePinType(g, n, false, static_cast<int>(i));
            meta.inputs[i] = t;
            if (t != PinType::Exec) out->literals[{n.id, static_cast<int>(i)}] = vscript::parseLiteral(n.inputs[i].value, t);
        }
        if (n.breakpoint) out->breakpoints.insert(n.id);
        if (n.disabled) continue;  // un evento desactivado no se lanza
        const vscript::NodeInfo* info = vscript::findNodeInfo(n.kind);
        if (info != nullptr && info->event) out->events[n.kind].push_back(&n);
        if (n.kind == "event.custom" && !n.inputs.empty()) {
            const std::string name = trimText(n.inputs[0].value);
            if (!name.empty() && !reserved(name)) out->custom[name] = &n;
        }
    }
    for (const vscript::Link& l : g.links) {
        out->input_links[{l.to_node, l.to_pin}] = {l.from_node, l.from_pin};
        const Node* from = out->node(l.from_node);
        if (from != nullptr && l.from_pin >= 0 && l.from_pin < static_cast<int>(from->outputs.size()) &&
            from->outputs[static_cast<std::size_t>(l.from_pin)].type == PinType::Exec) {
            out->output_links[{l.from_node, l.from_pin}] = {l.to_node, l.to_pin};
        }
    }
    return out;
}

// --- Instancias ----------------------------------------------------------------------

// Por donde iba una cadena cortada por un Delay (de dentro afuera).
struct Frame {
    int node = 0;
    long long next = 0;   // Sequence: siguiente salida; For / For Each: siguiente vuelta; While: vueltas
    long long last = 0;   // For: hasta; While: limite
    api::Value list;      // For Each
};

struct Pending {
    std::vector<Frame> frames;
    double wait = 0.0;
};

struct Timer {
    double left = 0.0;
    double every = 0.0;  // 0 = una vez
};

struct Instance {
    std::shared_ptr<const Loaded> graph;
    entt::entity entity = entt::null;
    bool started = false;
    bool failed = false;
    std::map<std::string, api::Value> vars;
    std::map<PinKey, api::Value> slots;   // ultimo valor de cada salida
    std::map<int, api::Value> state;      // Do Once, Do N, Flip Flop, Gate
    std::map<int, double> executed;       // nodo -> Time.time
    std::vector<Pending> pending;
    std::map<std::string, Timer> timers;
    std::vector<long long> bindings;      // Input.bindAction
    std::vector<long long> ws_listeners;  // WebSocket.on (On WebSocket)
};

// Corta la cadena (Delay): no es un error.
struct Suspend {};
// Un evento llamado desde la cadena (Call Event) fallo y ya lo dijo: se deshace sin repetirlo.
struct Abort {};

// Una ejecucion: la instancia, el nodo que corre (para los errores) y, si se
// corta, por donde iba.
struct Exec {
    Instance& inst;
    int node = 0;
    int depth = 0;
    std::vector<Frame> frames;  // al cortarse: de dentro afuera
    double wait = 0.0;
};

struct Host final : VisualScriptDebugHost {
    Runtime& rt;
    explicit Host(Runtime& r) : rt(r) {}

    std::unordered_map<std::string, std::shared_ptr<const Loaded>> graphs;
    std::unordered_map<std::string, std::string> failed;  // .crgraph -> error (hasta recargar)
    std::map<entt::entity, Instance> instances;
    std::map<std::string, std::set<int>> editor_breakpoints;
    bool debugging = false;
    struct Break {
        bool pending = false;
        std::string graph;
        int node = 0;
        entt::entity entity = entt::null;
    } brk;
    std::mt19937 rng{std::random_device{}()};
    // Callbacks locales (Input.bindAction): id -> (objeto, nodo Input Action).
    std::uint64_t next_callback = api::NativeApi::kLocalCallbackBase;
    std::unordered_map<std::uint64_t, std::pair<entt::entity, int>> callbacks;

    // --- Cargar ---
    std::shared_ptr<const Loaded> load(const std::string& file, std::string& error) {
        Graph graph;
        std::string problem;
        if (!vscript::loadGraph(rt.root / pathFromUtf8(file), graph, &problem)) {
            error = file + ": " + problem;
            return nullptr;
        }
        const vscript::CompileResult compiled = vscript::compileGraph(graph, file);
        if (!compiled.ok) {
            error = file + ": el grafo tiene errores (abrelo en la ventana Visual Script)";
            for (std::size_t i = 0; i < compiled.errors.size() && i < 4; ++i) {
                const vscript::NodeError& e = compiled.errors[i];
                error += "\n  " + (e.node > 0 ? "nodo " + std::to_string(e.node) + ": " : std::string()) + e.message;
            }
            return nullptr;
        }
        return prepare(file, std::move(graph));
    }

    std::shared_ptr<const Loaded> graphFor(const std::string& file) {
        if (const auto it = graphs.find(file); it != graphs.end()) return it->second;
        if (failed.contains(file)) return nullptr;
        std::string error;
        std::shared_ptr<const Loaded> g = load(file, error);
        if (!g) {
            failed[file] = error;
            rt.fail(file, error);
            return nullptr;
        }
        return graphs[file] = g;
    }

    // --- Valores ---
    api::Value self(const Instance& inst) const { return rt.entityValue(inst.entity); }

    // El objeto de la instancia (vacio si ya no existe).
    ecs::Entity selfEntity(const Instance& inst) const {
        if (rt.world == nullptr || !rt.world->registry().valid(inst.entity)) return {};
        return rt.world->wrap(inst.entity);
    }

    // `e` o alguien de debajo con ese nombre (en profundidad).
    static ecs::Entity findInTree(const ecs::Entity& e, const std::string& name) {
        if (!e.valid() || e.world() == nullptr) return {};
        if (e.name() == name) return e;
        for (const entt::entity child : e.children()) {
            const ecs::Entity found = findInTree(e.world()->wrap(child), name);
            if (found.valid()) return found;
        }
        return {};
    }

    // Un objeto por su nombre: primero este objeto y lo de debajo, luego su
    // prefab / Blueprint y por ultimo el mundo. Asi dos instancias del mismo
    // Blueprint usan cada una su "Mano_R".
    api::Value findObject(const Instance& inst, const std::string& name) {
        if (rt.world == nullptr || name.empty()) return {};
        const ecs::Entity me = selfEntity(inst);
        if (me.valid()) {
            if (const ecs::Entity e = findInTree(me, name); e.valid()) return rt.entityValue(e);
            const ecs::Entity root = ecs::prefabRoot(me);
            if (root.valid() && root.handle() != me.handle()) {
                if (const ecs::Entity e = findInTree(root, name); e.valid()) return rt.entityValue(e);
            }
        }
        const ecs::Entity e = rt.world->findByName(name);
        return e.valid() ? rt.entityValue(e) : api::Value{};
    }

    // Nodo "entity.child": la ruta desde la raiz de esta instancia (la de su
    // prefab / Blueprint, o el propio objeto). Si la ruta ya no existe (se
    // renombro o se movio un padre), el primero con ese nombre debajo de Self
    // y luego debajo de la raiz. Nunca el resto del mundo: es de este actor.
    api::Value childByPath(const Instance& inst, const std::string& path) {
        const ecs::Entity me = selfEntity(inst);
        if (!me.valid()) return {};
        const ecs::Entity owner = ecs::prefabRoot(me);
        const ecs::Entity root = owner.valid() ? owner : me;
        if (const ecs::Entity e = vscript::resolveEntityChildPath(root, path); e.valid()) return rt.entityValue(e);
        if (root.handle() != me.handle()) {
            if (const ecs::Entity e = vscript::resolveEntityChildPath(me, path); e.valid()) return rt.entityValue(e);
        }
        std::string name = vscript::entityChildPathName(path);
        if (const std::size_t mark = name.rfind(" ["); mark != std::string::npos && !name.empty() && name.back() == ']') {
            name.erase(mark);
        }
        if (name.empty()) return {};
        for (const ecs::Entity& from : {me, root}) {
            for (const entt::entity child : from.children()) {
                if (const ecs::Entity e = findInTree(from.world()->wrap(child), name); e.valid()) return rt.entityValue(e);
            }
        }
        return {};
    }

    api::Value literal(const Instance& inst, const Literal& lit) {
        switch (lit.kind) {
            case Literal::Kind::Nil: return {};
            case Literal::Kind::Bool: return api::Value(lit.boolean);
            case Literal::Kind::Number: return api::Value(lit.number);
            case Literal::Kind::Text: return api::Value(lit.text);
            case Literal::Kind::Vector: return api::Value(lit.vector);
            case Literal::Kind::Self: return self(inst);
            case Literal::Kind::Find: return findObject(inst, lit.text);
        }
        return {};
    }

    // Variables que faltan (al crear la instancia y al recargar el grafo):
    // las del grafo con su valor inicial; las entity escritas como texto se
    // buscan por nombre. Y el estado inicial de Do Once y Gate.
    void init(Instance& inst) {
        const Loaded& g = *inst.graph;
        for (const vscript::Variable& v : g.graph.variables) {
            if (v.name.empty()) continue;
            auto it = inst.vars.find(v.name);
            if (it == inst.vars.end()) {
                if (v.type == PinType::Entity) {
                    it = inst.vars.emplace(v.name, api::Value(trimText(v.value))).first;
                } else {
                    it = inst.vars.emplace(v.name, literal(inst, vscript::parseLiteral(v.value, v.type))).first;
                }
            }
            if (v.type == PinType::Entity && it->second.isString()) it->second = findObject(inst, it->second.asString());
        }
        for (const Node& n : g.graph.nodes) {
            if (n.kind != "flow.do_once" && n.kind != "flow.gate") continue;
            if (inst.state.contains(n.id)) continue;
            Exec x{inst};
            try {
                if (n.kind == "flow.do_once" && input(x, n, 2).truthy()) inst.state[n.id] = api::Value(true);
                if (n.kind == "flow.gate") inst.state[n.id] = api::Value(!input(x, n, 4).truthy());
            } catch (const api::Error&) {
            }
        }
    }

    static api::Value propertyValue(const ScriptProperty& p) {
        switch (p.type) {
            case PropertyType::Number: {
                double n = 0.0;
                return numberFromText(p.value, n) ? api::Value(n) : api::Value(0.0);
            }
            case PropertyType::Bool: {
                const std::string l = lowerText(trimText(p.value));
                return api::Value(l == "true" || l == "1");
            }
            case PropertyType::Vector: return api::Value(vscript::parseLiteral(p.value, PinType::Vector).vector);
            case PropertyType::Text: return api::Value(p.value);
        }
        return {};
    }

    void create(ecs::Entity e, const vscript::VisualScript& component) {
        if (component.graph.empty()) return;
        std::shared_ptr<const Loaded> g = graphFor(component.graph);
        if (!g) return;
        Instance inst;
        inst.graph = std::move(g);
        inst.entity = e.handle();
        // Lo del Inspector de este objeto; el resto, lo del grafo (init).
        for (const ScriptProperty& p : component.properties) {
            if (!p.name.empty()) inst.vars[p.name] = propertyValue(p);
        }
        init(inst);
        instances[e.handle()] = std::move(inst);
    }

    bool runnable(entt::entity h) const {
        if (rt.world == nullptr || !rt.world->registry().valid(h)) return false;
        const auto* c = rt.world->registry().try_get<vscript::VisualScript>(h);
        return c != nullptr && c->enabled && rt.world->wrap(h).activeInHierarchy();
    }

    Instance* live(entt::entity h) {
        const auto it = instances.find(h);
        if (it == instances.end() || it->second.failed || !runnable(h)) return nullptr;
        return &it->second;
    }

    // Objetos con Visual Script nuevos (creados en Play) y los que ya no estan.
    void sync() {
        if (rt.world == nullptr) return;
        auto& registry = rt.world->registry();
        for (auto it = instances.begin(); it != instances.end();) {
            const auto* c = registry.valid(it->first) ? registry.try_get<vscript::VisualScript>(it->first) : nullptr;
            if (c == nullptr || c->graph != it->second.graph->file) {
                forgetBindings(it->second);
                it = instances.erase(it);
            } else {
                ++it;
            }
        }
        std::vector<entt::entity> fresh;
        for (const entt::entity h : registry.view<vscript::VisualScript>()) {
            if (!instances.contains(h)) fresh.push_back(h);
        }
        std::sort(fresh.begin(), fresh.end());
        for (const entt::entity h : fresh) {
            const ecs::Entity e = rt.world->wrap(h);
            if (e.activeInHierarchy()) create(e, e.get<vscript::VisualScript>());
        }
    }

    std::vector<entt::entity> order() const {
        std::vector<entt::entity> out;
        out.reserve(instances.size());
        for (const auto& [h, inst] : instances) out.push_back(h);
        return out;
    }

    // --- Errores y depuracion ---
    void fault(Instance& inst, int node, const std::string& message) {
        inst.failed = true;  // hasta que se recargue el grafo
        rt.fail(inst.graph->file, inst.graph->file + ": " + (node > 0 ? "nodo " + std::to_string(node) + " (" +
                                                                            nodeName(*inst.graph, node) + "): "
                                                                      : std::string()) +
                                      message);
    }
    static std::string nodeName(const Loaded& g, int id) {
        const Node* n = g.node(id);
        return n != nullptr ? vscript::nodeTitle(*n) : std::string("?");
    }

    void trace(Exec& x, int node) {
        x.node = node;
        x.inst.executed[node] = rt.time;
        if (!debugging || brk.pending) return;
        const std::string& file = x.inst.graph->file;
        const auto editor = editor_breakpoints.find(file);  // las del editor mandan
        const std::set<int>& points = editor != editor_breakpoints.end() ? editor->second : x.inst.graph->breakpoints;
        if (!points.contains(node)) return;
        brk.pending = true;
        brk.graph = file;
        brk.node = node;
        brk.entity = x.inst.entity;
        rt.write(1, "Visual Script: punto de ruptura en " + file + " (nodo " + std::to_string(node) + ")");
    }

    // --- Datos ---
    static double number(const api::Value& v, const char* what) {
        if (v.isNumber()) return v.asNumber();
        double n = 0.0;
        if (v.isString() && numberFromText(v.asString(), n)) return n;
        throw api::Error(std::string(what) + ": se esperaba un numero (llego " + describe(v) + ")");
    }
    // tonumber(x) or 0
    static double lenient(const api::Value& v, double fallback = 0.0) {
        if (v.isNumber()) return v.asNumber();
        double n = 0.0;
        if (v.isString() && numberFromText(v.asString(), n)) return n;
        return fallback;
    }
    static core::Vec3 vector(const api::Value& v, const char* what) {
        if (v.isVec3()) return v.asVec3();
        throw api::Error(std::string(what) + ": se esperaba un Vec3 (llego " + describe(v) + ")");
    }
    static std::string describe(const api::Value& v) {
        switch (v.type()) {
            case api::Value::Type::Nil: return "nil";
            case api::Value::Type::Bool: return "un bool";
            case api::Value::Type::Number: return "un numero";
            case api::Value::Type::String: return "un texto";
            case api::Value::Type::Vec3: return "un Vec3";
            case api::Value::Type::Quat: return "un Quat";
            case api::Value::Type::Entity: return "un objeto";
            case api::Value::Type::Array: return "una lista";
            case api::Value::Type::Object: return "un objeto de datos";
            case api::Value::Type::Function: return "una funcion";
            case api::Value::Type::Handle: return "un objeto del motor";
        }
        return "?";
    }
    // STR(v): nil -> "", lo demas como texto.
    static std::string text(const api::Value& v) { return v.isNil() ? std::string() : v.asString(); }
    static bool missing(const api::Value& v) { return v.isNil() || (v.isEntity() && v.asEntity() == entt::null); }

    static api::Value arith(char op, const api::Value& a, const api::Value& b) {
        const char* what = op == '+' ? "Sumar" : op == '-' ? "Restar" : op == '*' ? "Multiplicar" : "Dividir";
        if (a.isVec3() || b.isVec3()) {
            if (a.isVec3() && b.isVec3()) {
                const core::Vec3 x = a.asVec3();
                const core::Vec3 y = b.asVec3();
                switch (op) {
                    case '+': return api::Value(x + y);
                    case '-': return api::Value(x - y);
                    case '*': return api::Value(x * y);
                    default: return api::Value(core::Vec3{x.x / y.x, x.y / y.y, x.z / y.z});
                }
            }
            if (op == '*' || op == '/') {
                const bool vec_first = a.isVec3();
                const core::Vec3 v = vec_first ? a.asVec3() : b.asVec3();
                const float s = static_cast<float>(number(vec_first ? b : a, what));
                if (op == '*') return api::Value(v * s);
                if (vec_first) return api::Value(v * (1.0f / s));
                return api::Value(core::Vec3{s / v.x, s / v.y, s / v.z});
            }
            throw api::Error(std::string(what) + ": no se puede con " + describe(a) + " y " + describe(b));
        }
        const double x = number(a, what);
        const double y = number(b, what);
        switch (op) {
            case '+': return api::Value(x + y);
            case '-': return api::Value(x - y);
            case '*': return api::Value(x * y);
            default: return api::Value(x / y);
        }
    }

    static bool equal(const api::Value& a, const api::Value& b) {
        if (a.type() != b.type()) return false;
        switch (a.type()) {
            case api::Value::Type::Nil: return true;
            case api::Value::Type::Bool:
            case api::Value::Type::Number: return a.asNumber() == b.asNumber() && a.truthy() == b.truthy();
            case api::Value::Type::String: return a.asString() == b.asString();
            case api::Value::Type::Vec3: {
                const core::Vec3 x = a.asVec3();
                const core::Vec3 y = b.asVec3();
                return x.x == y.x && x.y == y.y && x.z == y.z;
            }
            case api::Value::Type::Quat: {
                const core::Quat x = a.asQuat();
                const core::Quat y = b.asQuat();
                return x.x == y.x && x.y == y.y && x.z == y.z && x.w == y.w;
            }
            case api::Value::Type::Entity: return a.asEntity() == b.asEntity();
            case api::Value::Type::Function: return a.callbackId() == b.callbackId();
            case api::Value::Type::Handle: return a.asHandle() == b.asHandle();
            default: return false;  // listas y objetos: solo si son el mismo (no se sabe aqui)
        }
    }

    // a < b (less) o a <= b, numeros o textos.
    static bool order(const api::Value& a, const api::Value& b, bool or_equal) {
        if (a.isNumber() && b.isNumber()) return or_equal ? a.asNumber() <= b.asNumber() : a.asNumber() < b.asNumber();
        if (a.isString() && b.isString()) return or_equal ? a.asString() <= b.asString() : a.asString() < b.asString();
        throw api::Error("no se puede comparar " + describe(a) + " con " + describe(b));
    }

    // Valor de la entrada `index` de `n`: lo que llega por su enlace o lo escrito.
    api::Value input(Exec& x, const Node& n, int index) {
        if (index < 0 || index >= static_cast<int>(n.inputs.size())) return {};
        const Loaded& g = *x.inst.graph;
        const auto meta = g.meta.find(n.id);
        const PinType want = meta != g.meta.end() && index < static_cast<int>(meta->second.inputs.size())
                                 ? meta->second.inputs[static_cast<std::size_t>(index)]
                                 : vscript::nodePinType(g.graph, n, false, index);
        const auto link = g.input_links.find({n.id, index});
        if (link == g.input_links.end()) {
            const auto lit = g.literals.find({n.id, index});
            return lit == g.literals.end() ? api::Value{} : literal(x.inst, lit->second);
        }
        const Node* src = g.node(link->second.first);
        if (src == nullptr) return {};
        api::Value v = output(x, *src, link->second.second);
        if (want == PinType::String && !v.isString()) {
            const PinType have = vscript::nodePinType(g.graph, *src, true, link->second.second);
            if (have != PinType::String && have != PinType::Any) v = api::Value(text(v));
        }
        return v;
    }

    api::Value output(Exec& x, const Node& n, int pin) {
        const auto meta = x.inst.graph->meta.find(n.id);
        const bool is_pure = meta != x.inst.graph->meta.end() ? meta->second.pure : vscript::isPureNode(n);
        if (!is_pure) {
            const auto it = x.inst.slots.find({n.id, pin});
            return it == x.inst.slots.end() ? api::Value{} : it->second;
        }
        if (n.disabled) return {};  // compileGraph no deja usar su valor
        if (++x.depth > 512) throw api::Error("demasiados nodos de datos encadenados");
        const int previous = x.node;
        x.node = n.id;
        api::Value v = pure(x, n, pin);
        x.node = previous;
        --x.depth;
        x.inst.slots[{n.id, pin}] = v;
        return v;
    }

    std::vector<int> dataInputs(const Node& n) const {
        std::vector<int> data;
        for (std::size_t i = 0; i < n.inputs.size(); ++i) {
            if (n.inputs[i].type != PinType::Exec) data.push_back(static_cast<int>(i));
        }
        return data;
    }

    // Llamada de un nodo "call": Tabla.funcion(args) o objeto:metodo(args).
    api::Value callNode(Exec& x, const Node& n) {
        const std::vector<int> data = dataInputs(n);
        const bool method = n.fn.find(':') != std::string::npos;
        api::Value target;
        std::size_t first = 0;
        if (method) {
            target = data.empty() ? self(x.inst) : input(x, n, data[0]);
            first = data.empty() ? 0 : 1;
            if (missing(target)) throw api::Error(n.fn + ": el objeto es nil");
        }
        api::Value::Array args;
        for (std::size_t k = first; k < data.size(); ++k) args.push_back(input(x, n, data[k]));
        while (!args.empty() && args.back().isNil()) args.pop_back();
        return rt.native.call(n.fn, args, target);
    }

    // call.get / call.set: Tabla.prop u objeto:prop.
    api::Value propertyTarget(Exec& x, const Node& n) {
        if (n.fn.find(':') == std::string::npos) return {};
        const std::vector<int> data = dataInputs(n);
        api::Value target = data.empty() ? self(x.inst) : input(x, n, data[0]);
        if (missing(target)) throw api::Error(n.fn + ": el objeto es nil");
        return target;
    }

    api::Value entityGet(Exec& x, const Node& n, const char* property) {
        const api::Value o = input(x, n, 0);
        if (missing(o)) throw api::Error(std::string(property) + ": el objeto es nil");
        return rt.native.get(std::string("Entity:") + property, o);
    }

    api::Value pure(Exec& x, const Node& n, int pin) {
        const std::string& k = n.kind;
        const auto a = [&](int i) { return input(x, n, i); };
        const auto num = [&](int i) { return number(a(i), vscript::nodeTitle(n).c_str()); };
        if (k == "var.get") {
            const auto it = x.inst.vars.find(n.fn);
            return it == x.inst.vars.end() ? api::Value{} : it->second;
        }
        if (k == "call") return callNode(x, n);
        if (k == "call.get") return rt.native.get(n.fn, propertyTarget(x, n));
        if (k == "math.add") return arith('+', a(0), a(1));
        if (k == "math.sub") return arith('-', a(0), a(1));
        if (k == "math.mul") return arith('*', a(0), a(1));
        if (k == "math.div") return arith('/', a(0), a(1));
        if (k == "math.mod") {
            const double p = num(0);
            const double q = num(1);
            return api::Value(p - std::floor(p / q) * q);  // con el signo del divisor
        }
        if (k == "math.pow") return api::Value(std::pow(num(0), num(1)));
        if (k == "math.min") return api::Value(std::min(num(0), num(1)));
        if (k == "math.max") return api::Value(std::max(num(0), num(1)));
        if (k == "math.clamp") return api::Value(std::min(std::max(num(0), num(1)), num(2)));
        if (k == "math.lerp") {
            const api::Value p = a(0);
            const api::Value q = a(1);
            return arith('+', p, arith('*', arith('-', q, p), a(2)));
        }
        if (k == "math.map_range") {
            const double v = num(0), p = num(1), q = num(2), r = num(3), s = num(4);
            return api::Value(q == p ? r : r + (v - p) / (q - p) * (s - r));
        }
        if (k == "math.abs") return api::Value(std::abs(num(0)));
        if (k == "math.sqrt") return api::Value(std::sqrt(num(0)));
        if (k == "math.floor") return api::Value(std::floor(num(0)));
        if (k == "math.ceil") return api::Value(std::ceil(num(0)));
        if (k == "math.round") return api::Value(std::floor(num(0) + 0.5));
        if (k == "math.sin") return api::Value(std::sin(num(0)));
        if (k == "math.cos") return api::Value(std::cos(num(0)));
        if (k == "math.tan") return api::Value(std::tan(num(0)));
        if (k == "math.negate") {
            const api::Value v = a(0);
            return v.isVec3() ? api::Value(-v.asVec3()) : api::Value(-number(v, "Negar"));
        }
        if (k == "math.atan2") return api::Value(std::atan2(num(0), num(1)));
        if (k == "math.random_float") {
            const double lo = num(0);
            const double hi = num(1);
            return api::Value(lo + std::uniform_real_distribution<double>(0.0, 1.0)(rng) * (hi - lo));
        }
        if (k == "math.random_int") {
            long long lo = static_cast<long long>(std::floor(num(0)));
            long long hi = static_cast<long long>(std::floor(num(1)));
            if (hi < lo) std::swap(lo, hi);
            return api::Value(static_cast<double>(std::uniform_int_distribution<long long>(lo, hi)(rng)));
        }
        if (k == "math.pi") return api::Value(3.14159265358979323846);
        if (k == "logic.and") {
            const api::Value p = a(0);
            return p.truthy() ? a(1) : p;
        }
        if (k == "logic.or") {
            const api::Value p = a(0);
            return p.truthy() ? p : a(1);
        }
        if (k == "logic.xor") return api::Value(a(0).truthy() != a(1).truthy());
        if (k == "logic.not") return api::Value(!a(0).truthy());
        if (k == "logic.equal") return api::Value(equal(a(0), a(1)));
        if (k == "logic.not_equal") return api::Value(!equal(a(0), a(1)));
        if (k == "logic.greater") return api::Value(order(a(1), a(0), false));
        if (k == "logic.less") return api::Value(order(a(0), a(1), false));
        if (k == "logic.greater_equal") return api::Value(order(a(1), a(0), true));
        if (k == "logic.less_equal") return api::Value(order(a(0), a(1), true));
        if (k == "logic.select") return a(0).truthy() ? a(1) : a(2);
        if (k == "vec.make") {
            return api::Value(core::Vec3{static_cast<float>(num(0)), static_cast<float>(num(1)), static_cast<float>(num(2))});
        }
        if (k == "vec.break") {
            const core::Vec3 v = vector(a(0), "Break Vector");
            return api::Value(pin == 0 ? v.x : (pin == 1 ? v.y : v.z));
        }
        if (k == "vec.length") return api::Value(core::length(vector(a(0), "Length")));
        if (k == "vec.normalize") return api::Value(core::normalize(vector(a(0), "Normalize")));
        if (k == "vec.distance") return api::Value(core::length(vector(a(0), "Distance") - vector(a(1), "Distance")));
        if (k == "vec.dot") return api::Value(core::dot(vector(a(0), "Dot"), vector(a(1), "Dot")));
        if (k == "vec.cross") return api::Value(core::cross(vector(a(0), "Cross"), vector(a(1), "Cross")));
        if (k == "vec.lerp") {
            return api::Value(core::lerp(vector(a(0), "Vector Lerp"), vector(a(1), "Vector Lerp"), static_cast<float>(num(2))));
        }
        if (k == "str.append") return api::Value(text(a(0)) + text(a(1)));
        if (k == "str.to_string") return api::Value(text(a(0)));
        if (k == "str.to_number") return api::Value(lenient(a(0)));
        if (k == "str.length") return api::Value(static_cast<double>(text(a(0)).size()));
        if (k == "str.contains") return api::Value(text(a(0)).find(text(a(1))) != std::string::npos);
        if (k == "entity.self") return self(x.inst);
        if (k == "entity.child") return childByPath(x.inst, n.fn);
        if (k == "entity.find")return rt.native.call("Scene.find", {a(0)});
        if (k == "entity.find_tag") return rt.native.call("Scene.findWithTag", {a(0)});
        if (k == "entity.get_position") return entityGet(x, n, "position");
        if (k == "entity.get_rotation") return entityGet(x, n, "rotation");
        if (k == "entity.get_scale") return entityGet(x, n, "scale");
        if (k == "entity.get_forward") return entityGet(x, n, "forward");
        if (k == "entity.get_name") return entityGet(x, n, "name");
        if (k == "entity.is_valid") {
            const api::Value o = a(0);
            return api::Value(!missing(o) && rt.native.call("Entity:valid", {}, o).truthy());
        }
        if (k == "entity.distance") {
            const api::Value o = a(0);
            if (missing(o)) throw api::Error("Distance To: el objeto A es nil");
            return rt.native.call("Entity:distanceTo", {a(1)}, o);
        }
        if (k == "entity.get_field") {
            const api::Value o = a(0);
            if (missing(o)) throw api::Error("Get Component Field: el objeto es nil");
            return rt.native.call("Entity:getField", {a(1), a(2)}, o);
        }
        if (k == "input.key") return rt.native.call("Input.getKey", {a(0)});
        if (k == "input.key_down") return rt.native.call("Input.getKeyDown", {a(0)});
        if (k == "input.key_up") return rt.native.call("Input.getKeyUp", {a(0)});
        if (k == "input.axis") return rt.native.call("Input.getAxis", {a(0)});
        if (k == "input.mouse_button") return rt.native.call("Input.getMouseButton", {a(0)});
        if (k == "input.action") return rt.native.call("Input.getAction", {a(0)});
        if (k == "time.delta") return api::Value(rt.delta);
        if (k == "time.time") return api::Value(rt.time);
        if (k == "time.frame") return api::Value(static_cast<double>(rt.frame));
        if (k == "util.reroute") return a(0);
        return pureMore(x, n, pin);
    }

    // Delta de un nodo Interp To: el del frame si la entrada esta vacia y sin enlace.
    double deltaInput(Exec& x, const Node& n, int index) {
        const Loaded& g = *x.inst.graph;
        if (!g.input_links.contains({n.id, index}) && index < static_cast<int>(n.inputs.size()) &&
            trimText(n.inputs[static_cast<std::size_t>(index)].value).empty()) {
            return rt.delta;
        }
        return number(input(x, n, index), "Delta");
    }

    static std::string upperText(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return s;
    }

    // Posicion de `v` en una lista (desde 1; 0 si no esta).
    static long long findIn(const api::Value& list, const api::Value& v) {
        if (!list.isArray()) return 0;
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (equal(list[i], v)) return static_cast<long long>(i) + 1;
        }
        return 0;
    }

    // Objeto de un Blueprint / prefab por el id de la entidad dentro del prefab
    // (PrefabLink.source) o por su nombre, dentro de esta instancia.
    api::Value componentRef(const Instance& inst, const std::string& key) {
        if (key.empty()) return {};
        const ecs::Entity me = selfEntity(inst);
        if (!me.valid()) return {};
        const ecs::Entity root = ecs::prefabRoot(me).valid() ? ecs::prefabRoot(me) : me;
        ecs::Entity by_name;
        std::vector<ecs::Entity> stack{root};
        while (!stack.empty()) {
            const ecs::Entity e = stack.back();
            stack.pop_back();
            if (const auto* link = e.tryGet<ecs::PrefabLink>(); link != nullptr && link->source == key) return rt.entityValue(e);
            if (!by_name.valid() && e.name() == key) by_name = e;
            const std::vector<entt::entity>& children = e.children();
            for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(e.world()->wrap(*it));
        }
        return by_name.valid() ? rt.entityValue(by_name) : api::Value{};
    }

    // Get Component: `o` o el primero de debajo que tiene ese componente.
    api::Value withComponent(const api::Value& o, const std::string& name) {
        if (missing(o) || !o.isEntity() || rt.world == nullptr) return {};
        const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find(name);
        const ecs::Entity start = rt.entity(o.asEntity());
        if (type == nullptr || !start.valid()) return {};
        std::vector<ecs::Entity> queue{start};
        for (std::size_t i = 0; i < queue.size(); ++i) {
            const ecs::Entity e = queue[i];
            if (type->has(*rt.world, e.handle())) return rt.entityValue(e);
            for (const entt::entity child : e.children()) queue.push_back(rt.world->wrap(child));
        }
        return {};
    }

    // Los nodos de la biblioteca de la 2.5 (como Unreal): matematicas, conversiones,
    // texto, listas, objeto y fisica.
    api::Value pureMore(Exec& x, const Node& n, int pin) {
        const std::string& k = n.kind;
        const auto a = [&](int i) { return input(x, n, i); };
        const auto num = [&](int i) { return number(a(i), vscript::nodeTitle(n).c_str()); };
        const auto vec = [&](int i) { return vector(a(i), vscript::nodeTitle(n).c_str()); };
        if (k == "flow.select_int") {
            const double i = std::floor(lenient(a(0), -1.0));
            if (i < 0.0 || i + 1.0 >= static_cast<double>(n.inputs.size())) return {};
            return a(static_cast<int>(i) + 1);
        }
        // --- Matematicas ---
        if (k == "math.finterp_to") {
            const double current = num(0), target = num(1), dt = deltaInput(x, n, 2), speed = num(3);
            if (speed <= 0.0) return api::Value(target);
            const double dist = target - current;
            if (dist * dist < 1e-8) return api::Value(target);
            return api::Value(current + dist * std::clamp(dt * speed, 0.0, 1.0));
        }
        if (k == "math.vinterp_to") {
            const core::Vec3 current = vec(0), target = vec(1);
            const double dt = deltaInput(x, n, 2), speed = num(3);
            if (speed <= 0.0) return api::Value(target);
            const core::Vec3 dist = target - current;
            if (core::dot(dist, dist) < 1e-8f) return api::Value(target);
            return api::Value(current + dist * static_cast<float>(std::clamp(dt * speed, 0.0, 1.0)));
        }
        if (k == "math.nearly_equal") return api::Value(std::abs(num(0) - num(1)) <= std::abs(num(2)));
        if (k == "math.in_range") {
            const double v = num(0), lo = num(1), hi = num(2);
            const bool low_ok = a(3).truthy() ? v >= lo : v > lo;
            const bool high_ok = a(4).truthy() ? v <= hi : v < hi;
            return api::Value(low_ok && high_ok);
        }
        if (k == "math.wrap") {
            const double v = num(0), lo = num(1), hi = num(2);
            const double range = hi - lo;
            if (range == 0.0) return api::Value(lo);
            double r = std::fmod(v - lo, range);
            if (r < 0.0) r += range;
            return api::Value(lo + r);
        }
        if (k == "math.sign") {
            const double v = num(0);
            return api::Value(v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0));
        }
        if (k == "math.frac") {
            const double v = num(0);
            return api::Value(v - std::floor(v));
        }
        if (k == "math.asin") return api::Value(std::asin(std::clamp(num(0), -1.0, 1.0)));
        if (k == "math.acos") return api::Value(std::acos(std::clamp(num(0), -1.0, 1.0)));
        if (k == "math.atan") return api::Value(std::atan(num(0)));
        if (k == "math.exp") return api::Value(std::exp(num(0)));
        if (k == "math.log") {
            const double v = num(0);
            if (v <= 0.0) throw api::Error("Log: el valor tiene que ser mayor que 0");
            return api::Value(std::log(v));
        }
        if (k == "math.deg_to_rad") return api::Value(num(0) * 3.14159265358979323846 / 180.0);
        if (k == "math.rad_to_deg") return api::Value(num(0) * 180.0 / 3.14159265358979323846);
        if (k == "math.random_bool") return api::Value(std::uniform_int_distribution<int>(0, 1)(rng) == 1);
        // --- Conversiones ---
        if (k == "conv.int_to_float") return api::Value(num(0));
        if (k == "conv.float_to_int") return api::Value(std::trunc(num(0)));
        if (k == "conv.bool_to_int") return api::Value(a(0).truthy() ? 1.0 : 0.0);
        // --- Texto ---
        if (k == "str.format") {
            const std::string format = text(a(0));
            std::string out;
            std::size_t at = 0;
            while (at < format.size()) {
                const std::size_t open = format.find('{', at);
                const std::size_t close = open == std::string::npos ? std::string::npos : format.find('}', open + 1);
                if (close == std::string::npos) {
                    out += format.substr(at);
                    break;
                }
                out += format.substr(at, open - at);
                const std::string name = trimText(format.substr(open + 1, close - open - 1));
                const int index = n.input(name);
                if (index >= 1) out += text(a(index));
                else out += format.substr(open, close - open + 1);  // sin entrada: se queda tal cual
                at = close + 1;
            }
            return api::Value(out);
        }
        if (k == "str.substring") {
            const std::string s = text(a(0));
            const double start = std::max(0.0, std::floor(lenient(a(1))));
            const double length = std::max(0.0, std::floor(lenient(a(2))));
            if (start >= static_cast<double>(s.size())) return api::Value(std::string());
            return api::Value(s.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(std::min(length, 1e9))));
        }
        if (k == "str.replace") {
            std::string s = text(a(0));
            const std::string from = text(a(1));
            const std::string to = text(a(2));
            if (from.empty()) return api::Value(s);
            for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
            return api::Value(s);
        }
        if (k == "str.split") {
            const std::string s = text(a(0));
            const std::string sep = text(a(1));
            api::Value::Array parts;
            if (sep.empty()) {
                parts.emplace_back(s);
            } else {
                std::size_t at = 0;
                for (std::size_t next = s.find(sep); next != std::string::npos; next = s.find(sep, at)) {
                    parts.emplace_back(s.substr(at, next - at));
                    at = next + sep.size();
                }
                parts.emplace_back(s.substr(at));
            }
            return api::Value(std::move(parts));
        }
        if (k == "str.upper") return api::Value(upperText(text(a(0))));
        if (k == "str.lower") return api::Value(lowerText(text(a(0))));
        if (k == "str.trim") return api::Value(trimText(text(a(0))));
        if (k == "str.starts_with") {
            const std::string s = text(a(0)), p = text(a(1));
            return api::Value(s.size() >= p.size() && s.compare(0, p.size(), p) == 0);
        }
        if (k == "str.ends_with") {
            const std::string s = text(a(0)), p = text(a(1));
            return api::Value(s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0);
        }
        if (k == "str.is_empty") return api::Value(text(a(0)).empty());
        // --- Listas (desde 1) ---
        if (k == "array.make") {
            api::Value::Array items;
            for (std::size_t i = 0; i < n.inputs.size(); ++i) items.push_back(a(static_cast<int>(i)));
            return api::Value(std::move(items));
        }
        if (k == "array.get") {
            const api::Value list = a(0);
            const double i = std::floor(lenient(a(1)));
            if (!list.isArray() || i < 1.0 || i > static_cast<double>(list.size())) return {};
            return list[static_cast<std::size_t>(i) - 1];
        }
        if (k == "array.length") {
            const api::Value list = a(0);
            return api::Value(static_cast<double>(list.isArray() ? list.size() : 0));
        }
        if (k == "array.contains") return api::Value(findIn(a(0), a(1)) > 0);
        if (k == "array.find") return api::Value(static_cast<double>(findIn(a(0), a(1))));
        // --- Objeto ---
        if (k == "entity.owner") {
            const api::Value o = a(0);
            if (missing(o) || !o.isEntity()) return {};
            const ecs::Entity e = rt.entity(o.asEntity());
            if (!e.valid()) return {};
            const ecs::Entity root = ecs::prefabRoot(e);
            return rt.entityValue(root.valid() ? root : e);
        }
        if (k == "component.get") {
            const api::Value found = withComponent(a(0), text(a(1)));
            return pin == 1 ? api::Value(!found.isNil()) : found;
        }
        if (k == "component.has") {
            const api::Value o = a(0);
            if (missing(o)) return api::Value(false);
            return rt.native.call("Entity:hasComponent", {api::Value(text(a(1)))}, o);
        }
        if (k == "component.ref") return componentRef(x.inst, trimText(text(a(0))));
        // --- Fisica ---
        if (k == "physics.break_hit") {
            const api::Value r = a(0);
            if (!r.isObject()) return pin == 0 ? api::Value(false) : api::Value{};
            switch (pin) {
                case 0: return api::Value(true);
                case 1: return r["entity"];
                case 2: return r["point"];
                case 3: return r["normal"];
                default: return r["distance"];
            }
        }
        // --- Vector ---
        if (k == "vec.rotate") return api::Value(ecs::quatRotate(ecs::quatFromEulerDegrees(vec(1)), vec(0)));
        if (k == "vec.unit_direction") return api::Value(core::normalize(vec(1) - vec(0)));
        if (k == "vec.distance2d") {
            const core::Vec3 p = vec(0), q = vec(1);
            return api::Value(std::sqrt((p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z)));
        }
        throw api::Error("el nodo " + vscript::nodeTitle(n) + " no da valores");
    }

    // --- Ejecucion ---
    // Sigue por la salida exec `out` de `n` (nada si no tiene enlace).
    void follow(Exec& x, const Node& n, int out) {
        const Loaded& g = *x.inst.graph;
        const auto it = g.output_links.find({n.id, out});
        if (it == g.output_links.end()) return;
        const Node* next = g.node(it->second.first);
        if (next == nullptr) return;
        if (++x.depth > 2000) throw api::Error("demasiados nodos seguidos (un bucle de ejecucion sin Delay?)");
        run(x, *next, it->second.second);
        --x.depth;
    }

    // Sigue por `out` y, si la cadena se corta ahi, apunta `frame` para seguir despues.
    void followOrSave(Exec& x, const Node& n, int out, const Frame& frame) {
        try {
            follow(x, n, out);
        } catch (const Suspend&) {
            x.frames.push_back(frame);
            throw;
        }
    }

    void setField(Exec& x, const Node& n, const char* property) {
        const api::Value o = input(x, n, 1);
        if (!missing(o)) rt.native.set(std::string("Entity:") + property, input(x, n, 2), o);
    }

    void callOn(Exec& x, const Node& n, const char* method, api::Value::Array args) {
        const api::Value o = input(x, n, 1);
        if (!missing(o)) rt.native.call(std::string("Entity:") + method, args, o);
    }

    void run(Exec& x, const Node& n, int in) {
        trace(x, n.id);
        Instance& inst = x.inst;
        const std::string& k = n.kind;
        const auto a = [&](int i) { return input(x, n, i); };
        if (n.disabled) {
            // Desactivado: la ejecucion pasa por su primera salida exec.
            for (std::size_t o = 0; o < n.outputs.size(); ++o) {
                if (n.outputs[o].type == PinType::Exec) {
                    follow(x, n, static_cast<int>(o));
                    break;
                }
            }
            return;
        }
        if (k == "flow.branch") {
            follow(x, n, a(1).truthy() ? 0 : 1);
        } else if (k == "flow.sequence") {
            sequence(x, n, 0);
        } else if (k == "flow.for") {
            forLoop(x, n, static_cast<long long>(std::floor(lenient(a(1)))), static_cast<long long>(std::floor(lenient(a(2)))));
        } else if (k == "flow.foreach") {
            forEach(x, n, a(1), 0);
        } else if (k == "flow.while") {
            whileLoop(x, n, 0, static_cast<long long>(lenient(a(2), 10000.0)));
        } else if (k == "flow.delay") {
            x.wait = lenient(a(1));
            x.frames.push_back(Frame{n.id});
            throw Suspend{};
        } else if (k == "flow.do_once") {
            if (in == 0) {
                if (inst.state[n.id].truthy()) return;
                inst.state[n.id] = api::Value(true);
                follow(x, n, 0);
            } else {
                inst.state.erase(n.id);
            }
        } else if (k == "flow.do_n") {
            if (in == 0) {
                double count = inst.state[n.id].asNumber();
                if (count >= lenient(a(2))) return;
                count += 1.0;
                inst.state[n.id] = api::Value(count);
                inst.slots[{n.id, 1}] = api::Value(count);
                follow(x, n, 0);
            } else {
                inst.state[n.id] = api::Value(0.0);
            }
        } else if (k == "flow.flip_flop") {
            const bool is_a = !inst.state[n.id].truthy();
            inst.state[n.id] = api::Value(is_a);
            inst.slots[{n.id, 2}] = api::Value(is_a);
            follow(x, n, is_a ? 0 : 1);
        } else if (k == "flow.gate") {
            api::Value& open = inst.state[n.id];
            if (in == 0) {
                if (open.truthy()) follow(x, n, 0);
            } else if (in == 1) {
                open = api::Value(true);
            } else if (in == 2) {
                open = api::Value(false);
            } else {
                open = api::Value(!open.truthy());
            }
        } else if (k == "flow.set_timer") {
            const double seconds = lenient(a(2));
            inst.timers[text(a(1))] = Timer{seconds, a(3).truthy() ? seconds : 0.0};
            follow(x, n, 0);
        } else if (k == "flow.clear_timer") {
            inst.timers.erase(text(a(1)));
            follow(x, n, 0);
        } else if (k == "flow.call_event") {
            const std::string name = text(a(1));
            const auto it = inst.graph->custom.find(name);
            if (it == inst.graph->custom.end()) throw api::Error("Call Event: no existe el evento " + name);
            const api::Value value = a(2);
            const int previous = x.node;
            event(inst, *it->second, {value});
            if (inst.failed) throw Abort{};
            x.node = previous;
            follow(x, n, 0);
        } else if (k == "flow.send_event") {
            const api::Value target = a(1);
            if (!missing(target) && target.isEntity()) send(rt.entity(target.asEntity()), text(a(2)), a(3));
            follow(x, n, 0);
        } else if (k == "var.set") {
            const api::Value v = a(1);
            inst.vars[n.fn] = v;
            inst.slots[{n.id, 1}] = v;
            follow(x, n, 0);
        } else if (k == "debug.print" || k == "debug.warn") {
            rt.native.call(k == "debug.print" ? "Debug.log" : "Debug.warn", {api::Value(text(a(1)))});
            follow(x, n, 0);
        } else if (k == "entity.set_position") {
            setField(x, n, "position");
            follow(x, n, 0);
        } else if (k == "entity.set_rotation") {
            setField(x, n, "rotation");
            follow(x, n, 0);
        } else if (k == "entity.set_scale") {
            setField(x, n, "scale");
            follow(x, n, 0);
        } else if (k == "entity.set_active") {
            setField(x, n, "active");
            follow(x, n, 0);
        } else if (k == "entity.translate" || k == "entity.rotate" || k == "entity.look_at") {
            callOn(x, n, k == "entity.translate" ? "translate" : (k == "entity.rotate" ? "rotate" : "lookAt"), {a(2)});
            follow(x, n, 0);
        } else if (k == "entity.add_force") {
            callOn(x, n, "addForce", {a(2), a(3)});
            follow(x, n, 0);
        } else if (k == "entity.spawn") {
            inst.slots[{n.id, 1}] = rt.native.call("Scene.instantiate", {a(1), a(2), a(3)});
            follow(x, n, 0);
        } else if (k == "entity.destroy") {
            callOn(x, n, "destroy", {});
            follow(x, n, 0);
        } else if (k == "entity.set_field") {
            callOn(x, n, "setField", {a(2), a(3), a(4)});
            follow(x, n, 0);
        } else if (k == "call") {
            api::Value r = callNode(x, n);
            if (const int result = n.output("Resultado"); result >= 0) inst.slots[{n.id, result}] = std::move(r);
            follow(x, n, 0);
        } else if (k == "call.set") {
            const std::vector<int> data = dataInputs(n);
            const api::Value value = data.empty() ? api::Value{} : input(x, n, data.back());
            rt.native.set(n.fn, value, propertyTarget(x, n));
            follow(x, n, 0);
        } else if (!runMore(x, n, in)) {
            throw api::Error("el nodo " + vscript::nodeTitle(n) + " no se puede ejecutar");
        }
    }

    // Lista que cambia un nodo de Array: si llega de una variable (Get), la
    // variable se queda con la copia nueva (las listas se comparten).
    void writeBack(Exec& x, const Node& n, int input_index, const api::Value& list) {
        const Loaded& g = *x.inst.graph;
        const auto link = g.input_links.find({n.id, input_index});
        if (link == g.input_links.end()) return;
        const Node* src = g.node(link->second.first);
        if (src != nullptr && src->kind == "var.get" && !src->fn.empty()) x.inst.vars[src->fn] = list;
    }

    static api::Value::Array copyOf(const api::Value& list) {
        return list.isArray() ? list.items() : api::Value::Array{};
    }

    // Componentes que un grafo no puede anadir ni quitar.
    static bool lockedComponent(const std::string& name) {
        static const std::set<std::string> kLocked = {"Transform", "Name", "NameComponent", "EntityInfo", "PrefabInstance",
                                                      "PrefabLink"};
        return kLocked.contains(name);
    }

    // Salida exec de un Switch con ese nombre (o Default, la 0).
    static int switchOutput(const Node& n, const std::string& name) {
        for (std::size_t o = 1; o < n.outputs.size(); ++o) {
            if (n.outputs[o].type == PinType::Exec && n.outputs[o].name == name) return static_cast<int>(o);
        }
        return 0;
    }

    bool broken(const Instance& inst, const Node& n) const {
        const auto it = inst.state.find(n.id);
        return it != inst.state.end() && it->second.truthy();
    }

    // Los nodos de ejecucion de la biblioteca de la 2.5 (false = no es de aqui).
    bool runMore(Exec& x, const Node& n, int in) {
        Instance& inst = x.inst;
        const std::string& k = n.kind;
        const auto a = [&](int i) { return input(x, n, i); };
        if (k == "flow.switch_int") {
            const double selector = std::floor(lenient(a(1)));
            char name[32];
            std::snprintf(name, sizeof(name), "%.0f", selector);
            follow(x, n, switchOutput(n, name));
        } else if (k == "flow.switch_string") {
            follow(x, n, switchOutput(n, text(a(1))));
        } else if (k == "flow.for_break") {
            if (in == 3) {
                inst.state[n.id] = api::Value(true);  // Break: el bucle lo mira en cada vuelta
                return true;
            }
            inst.state.erase(n.id);
            forLoop(x, n, static_cast<long long>(std::floor(lenient(a(1)))), static_cast<long long>(std::floor(lenient(a(2)))));
        } else if (k == "flow.foreach_break") {
            if (in == 2) {
                inst.state[n.id] = api::Value(true);
                return true;
            }
            inst.state.erase(n.id);
            forEach(x, n, a(1), 0);
        } else if (k == "flow.multigate") {
            if (in == 1) {
                inst.state.erase(n.id);  // Reset
                return true;
            }
            const int outs = static_cast<int>(std::min<std::size_t>(n.outputs.size(), 52));
            if (outs == 0) return true;
            const api::Value previous = inst.state.contains(n.id) ? inst.state[n.id] : api::Value{};
            std::uint64_t used = static_cast<std::uint64_t>(previous["used"].asNumber());
            int next = 0;
            if (previous.isNil()) {
                const double start = std::floor(lenient(a(4), -1.0));
                next = start >= 0.0 && start < outs ? static_cast<int>(start) : 0;
            } else {
                next = static_cast<int>(previous["next"].asNumber()) % outs;
            }
            const std::uint64_t all = outs >= 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << outs) - 1;
            if ((used & all) == all) {
                if (!a(3).truthy()) return true;  // ya salio por todas (hasta Reset)
                used = 0;
            }
            int chosen = -1;
            if (a(2).truthy()) {
                std::vector<int> unused;
                for (int o = 0; o < outs; ++o) {
                    if ((used & (std::uint64_t(1) << o)) == 0) unused.push_back(o);
                }
                chosen = unused[std::uniform_int_distribution<std::size_t>(0, unused.size() - 1)(rng)];
            } else {
                for (int step = 0; step < outs && chosen < 0; ++step) {
                    const int o = (next + step) % outs;
                    if ((used & (std::uint64_t(1) << o)) == 0) chosen = o;
                }
            }
            used |= std::uint64_t(1) << chosen;
            api::Value saved = api::Value::object();
            saved.set("used", api::Value(static_cast<double>(used)));
            saved.set("next", api::Value(static_cast<double>((chosen + 1) % outs)));
            inst.state[n.id] = saved;
            follow(x, n, chosen);
        } else if (k == "flow.retrig_delay") {
            // Cada entrada empieza la cuenta otra vez: las esperas de antes se descartan.
            const double generation = (inst.state.contains(n.id) ? inst.state[n.id].asNumber() : 0.0) + 1.0;
            inst.state[n.id] = api::Value(generation);
            std::erase_if(inst.pending, [&](const Pending& p) { return !p.frames.empty() && p.frames.front().node == n.id; });
            x.wait = lenient(a(1));
            x.frames.push_back(Frame{n.id, static_cast<long long>(generation)});
            throw Suspend{};
        } else if (k == "flow.is_valid") {
            const api::Value o = a(1);
            const bool valid = !missing(o) && (!o.isEntity() || rt.native.call("Entity:valid", {}, o).truthy());
            follow(x, n, valid ? 0 : 1);
        } else if (k == "array.add") {
            api::Value::Array items = copyOf(a(1));
            items.push_back(a(2));
            const double index = static_cast<double>(items.size());
            const api::Value list(std::move(items));
            writeBack(x, n, 1, list);
            inst.slots[{n.id, 1}] = list;
            inst.slots[{n.id, 2}] = api::Value(index);
            follow(x, n, 0);
        } else if (k == "array.remove_index") {
            api::Value::Array items = copyOf(a(1));
            const double i = std::floor(lenient(a(2)));
            if (i >= 1.0 && i <= static_cast<double>(items.size())) items.erase(items.begin() + static_cast<std::ptrdiff_t>(i) - 1);
            const api::Value list(std::move(items));
            writeBack(x, n, 1, list);
            inst.slots[{n.id, 1}] = list;
            follow(x, n, 0);
        } else if (k == "array.clear") {
            const api::Value list(api::Value::Array{});
            writeBack(x, n, 1, list);
            inst.slots[{n.id, 1}] = list;
            follow(x, n, 0);
        } else if (k == "component.add" || k == "component.remove") {
            const bool add = k == "component.add";
            const api::Value o = a(1);
            const std::string name = trimText(text(a(2)));
            bool done = false;
            if (!missing(o) && !name.empty()) {
                const bool on_self = o.isEntity() && o.asEntity() == inst.entity;
                if (lockedComponent(name) || (on_self && name == "VisualScript")) {
                    rt.write(1, std::string(add ? "Add" : "Remove") + " Component: " + name + " no se puede cambiar desde un grafo");
                } else {
                    done = rt.native.call(add ? "Entity:addComponent" : "Entity:removeComponent", {api::Value(name)}, o).truthy();
                }
            }
            inst.slots[{n.id, 1}] = api::Value(done);
            follow(x, n, 0);
        } else if (k == "physics.line_trace") {
            const core::Vec3 from = vector(a(1), "Line Trace");
            const core::Vec3 to = vector(a(2), "Line Trace");
            const core::Vec3 direction = to - from;
            const float distance = core::length(direction);
            api::Value result;
            if (rt.physics != nullptr && distance > 1e-6f) {
                physics::QueryFilter filter;
                if (a(3).truthy()) filter.ignore = selfEntity(inst);  // no tocarse a si mismo
                physics::RaycastHit hit;
                if (rt.physics->raycast(from, direction * (1.0f / distance), distance, hit, filter)) {
                    result = api::Value::object();
                    result.set("entity", rt.entityValue(hit.entity));
                    result.set("point", hit.point);
                    result.set("normal", hit.normal);
                    result.set("distance", hit.distance);
                }
            }
            inst.slots[{n.id, 1}] = api::Value(!result.isNil());
            inst.slots[{n.id, 2}] = result["entity"];
            inst.slots[{n.id, 3}] = result["point"];
            inst.slots[{n.id, 4}] = result["normal"];
            inst.slots[{n.id, 5}] = result["distance"];
            inst.slots[{n.id, 6}] = result;
            follow(x, n, 0);
        } else {
            return false;
        }
        return true;
    }

    void sequence(Exec& x, const Node& n, long long from) {
        for (long long o = from; o < static_cast<long long>(n.outputs.size()); ++o) {
            followOrSave(x, n, static_cast<int>(o), Frame{n.id, o + 1});
        }
    }

    void forLoop(Exec& x, const Node& n, long long from, long long last) {
        const bool breakable = n.kind == "flow.for_break";  // For Loop with Break
        for (long long i = from; i <= last; ++i) {
            if (breakable && broken(x.inst, n)) break;
            x.inst.slots[{n.id, 1}] = api::Value(static_cast<double>(i));
            followOrSave(x, n, 0, Frame{n.id, i + 1, last});
        }
        if (breakable) x.inst.state.erase(n.id);
        follow(x, n, 2);
    }

    void forEach(Exec& x, const Node& n, const api::Value& list, long long from) {
        const bool breakable = n.kind == "flow.foreach_break";  // For Each with Break
        if (list.isArray()) {
            for (long long i = from; i < static_cast<long long>(list.size()); ++i) {
                if (breakable && broken(x.inst, n)) break;
                x.inst.slots[{n.id, 1}] = list[static_cast<std::size_t>(i)];
                x.inst.slots[{n.id, 2}] = api::Value(static_cast<double>(i + 1));  // desde 1, como la API
                Frame f{n.id, i + 1};
                f.list = list;
                followOrSave(x, n, 0, f);
            }
        }
        if (breakable) x.inst.state.erase(n.id);
        follow(x, n, 3);
    }

    void whileLoop(Exec& x, const Node& n, long long count, long long limit) {
        while (input(x, n, 1).truthy()) {
            if (++count > limit) {
                throw api::Error("While Loop: mas de " + std::to_string(limit) + " vueltas (bucle infinito?)");
            }
            followOrSave(x, n, 0, Frame{n.id, count, limit});
        }
        follow(x, n, 1);
    }

    // Sigue una cadena cortada: cada frame, de dentro afuera.
    void resume(Exec& x, const std::vector<Frame>& frames) {
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const Frame& f = frames[i];
            const Node* n = x.inst.graph->node(f.node);
            if (n == nullptr) return;  // el grafo cambio y ese nodo ya no esta
            try {
                x.node = n->id;
                if (n->kind == "flow.delay") follow(x, *n, 0);
                else if (n->kind == "flow.retrig_delay") {
                    // Una espera que se volvio a lanzar: esta cadena ya no sigue (la nueva si).
                    const auto it = x.inst.state.find(n->id);
                    if (it == x.inst.state.end() || static_cast<long long>(it->second.asNumber()) != f.next) return;
                    follow(x, *n, 0);
                }
                else if (n->kind == "flow.sequence") sequence(x, *n, f.next);
                else if (n->kind == "flow.for" || n->kind == "flow.for_break") forLoop(x, *n, f.next, f.last);
                else if (n->kind == "flow.foreach" || n->kind == "flow.foreach_break") forEach(x, *n, f.list, f.next);
                else if (n->kind == "flow.while") whileLoop(x, *n, f.next, f.last);
            } catch (const Suspend&) {
                // Se corta otra vez: lo de fuera sigue esperando detras.
                x.frames.insert(x.frames.end(), frames.begin() + static_cast<std::ptrdiff_t>(i) + 1, frames.end());
                throw;
            }
        }
    }

    // Lanza un evento: pone sus salidas de datos y sigue por su exec.
    void event(Instance& inst, const Node& n, const api::Value::Array& args) {
        if (inst.failed) return;
        Exec x{inst};
        try {
            trace(x, n.id);
            if (n.kind.rfind("event.collision", 0) == 0 && n.outputs.size() > 4) {
                const api::Value& contact = args.size() > 1 ? args[1] : api::Value::nil();
                inst.slots[{n.id, 1}] = args.empty() ? api::Value{} : args[0];
                inst.slots[{n.id, 2}] = contact["point"];
                inst.slots[{n.id, 3}] = contact["normal"];
                inst.slots[{n.id, 4}] = contact["relativeVelocity"];
            } else if (n.kind == "event.websocket") {
                // Evento, Texto (text, error o reason) y Datos (el JSON ya leido).
                const api::Value& ev = args.empty() ? api::Value::nil() : args[0];
                inst.slots[{n.id, 1}] = ev;
                api::Value text;
                for (const char* key : {"text", "error", "reason"}) {
                    if (!ev[key].isNil()) {
                        text = ev[key];
                        break;
                    }
                }
                inst.slots[{n.id, 2}] = text;
                inst.slots[{n.id, 3}] = ev["data"];
            } else if (n.kind == "event.video") {
                // Evento, Tiempo, Cantidad y Mensaje ({type, time, count, message} de VideoApi.cpp).
                const api::Value& ev = args.empty() ? api::Value::nil() : args[0];
                inst.slots[{n.id, 1}] = ev["type"];
                inst.slots[{n.id, 2}] = ev["time"];
                inst.slots[{n.id, 3}] = ev["count"];
                inst.slots[{n.id, 4}] = ev["message"];
            } else if (n.outputs.size() > 1) {
                inst.slots[{n.id, 1}] = args.empty() ? api::Value{} : args[0];
            }
            follow(x, n, 0);
        } catch (const Suspend&) {
            inst.pending.push_back(Pending{std::move(x.frames), x.wait});
        } catch (const Abort&) {
        } catch (const api::Error& e) {
            fault(inst, x.node, e.what());
        } catch (const std::exception& e) {
            fault(inst, x.node, e.what());
        }
    }

    void events(Instance& inst, const char* kind, const api::Value::Array& args = {}) {
        // La lista puede cambiar si el grafo se recarga: se copia.
        const std::shared_ptr<const Loaded> g = inst.graph;
        for (const Node* n : g->eventsOf(kind)) {
            if (inst.failed) return;
            event(inst, *n, args);
        }
    }

    // Esperas que terminan y temporizadores (antes de los Update).
    void tick(Instance& inst, double dt) {
        if (!inst.pending.empty()) {
            std::vector<Pending> list;
            list.swap(inst.pending);
            for (Pending& p : list) {
                if (inst.failed) return;
                p.wait -= dt;
                if (p.wait > 0.0) {
                    inst.pending.push_back(std::move(p));
                    continue;
                }
                Exec x{inst};
                try {
                    resume(x, p.frames);
                } catch (const Suspend&) {
                    inst.pending.push_back(Pending{std::move(x.frames), x.wait});
                } catch (const Abort&) {
                } catch (const api::Error& e) {
                    fault(inst, x.node, e.what());
                } catch (const std::exception& e) {
                    fault(inst, x.node, e.what());
                }
            }
        }
        std::vector<std::string> fired;
        for (auto it = inst.timers.begin(); it != inst.timers.end();) {
            it->second.left -= dt;
            if (it->second.left > 0.0) {
                ++it;
                continue;
            }
            fired.push_back(it->first);
            if (it->second.every > 0.0) {
                it->second.left += it->second.every;
                ++it;
            } else {
                it = inst.timers.erase(it);
            }
        }
        for (const std::string& name : fired) customEvent(inst, name, {});
    }

    // On Video Event: los OnVideo* del VideoPlayer del objeto (Evento = el tipo, o Any).
    void videoEvent(Instance& inst, const std::string& method, const api::Value& value) {
        if (inst.failed || method.rfind("OnVideo", 0) != 0) return;
        const std::shared_ptr<const Loaded> g = inst.graph;
        const std::string type = lowerText(method.substr(7));
        for (const Node* n : g->eventsOf("event.video")) {
            if (inst.failed) return;
            std::string want = !n->inputs.empty() ? lowerText(trimText(n->inputs[0].value)) : std::string();
            if (want.rfind("onvideo", 0) == 0) want = want.substr(7);
            if (!want.empty() && want != "any" && want != "cualquiera" && want != type) continue;
            event(inst, *n, {value});
        }
    }

    // Un Custom Event por su nombre (false si el grafo no lo tiene).
    bool customEvent(Instance& inst, const std::string& name, const api::Value& value) {
        if (inst.failed) return false;
        const std::shared_ptr<const Loaded> g = inst.graph;
        const auto it = g->custom.find(name);
        if (it == g->custom.end()) return false;
        event(inst, *it->second, {value});
        return true;
    }

    // Send Event: el Custom Event del Visual Script del objeto y el mensaje a sus scripts de C++.
    void send(ecs::Entity target, const std::string& method, const api::Value& value) {
        if (!target.valid() || method.empty()) return;
        for (const auto& fn : rt.hooks.event) fn(target, method, value);
        if (rt.message_listener) rt.message_listener(target, method, rt.native.toJson(value).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }

    // --- Fases ---
    void start(Instance& inst) {
        inst.started = true;
        const std::shared_ptr<const Loaded> g = inst.graph;
        for (const Node* n : g->eventsOf("event.input_action")) {
            const std::string action = !n->inputs.empty() ? n->inputs[0].value : std::string("Jump");
            const std::string when =
                n->inputs.size() > 1 && !trimText(n->inputs[1].value).empty() ? trimText(n->inputs[1].value) : "triggered";
            const std::uint64_t id = next_callback++;
            callbacks[id] = {inst.entity, n->id};
            try {
                const api::Value r = rt.native.call("Input.bindAction", {api::Value(action), api::Value(when), api::Value::function(id)});
                if (r.asNumber() > 0) inst.bindings.push_back(static_cast<long long>(r.asNumber()));
            } catch (const api::Error& e) {
                fault(inst, n->id, e.what());
                return;
            }
        }
        // On WebSocket: escucha la conexion por su nombre (aunque aun no exista).
        for (const Node* n : g->eventsOf("event.websocket")) {
            const std::string name =
                !n->inputs.empty() && !trimText(n->inputs[0].value).empty() ? trimText(n->inputs[0].value) : std::string("main");
            const std::string kind = n->inputs.size() > 1 && !trimText(n->inputs[1].value).empty()
                                         ? lowerText(trimText(n->inputs[1].value))
                                         : std::string("message");
            const std::uint64_t id = next_callback++;
            callbacks[id] = {inst.entity, n->id};
            try {
                const api::Value r = rt.native.call("WebSocket.on", {api::Value(name), api::Value(kind), api::Value::function(id)});
                if (r.asNumber() > 0) inst.ws_listeners.push_back(static_cast<long long>(r.asNumber()));
            } catch (const api::Error& e) {
                fault(inst, n->id, e.what());
                return;
            }
        }
        events(inst, "event.start");
    }

    void update(Instance& inst, float dt) {
        tick(inst, dt);
        const std::shared_ptr<const Loaded> g = inst.graph;
        for (const Node* n : g->eventsOf("event.key")) {
            if (inst.failed) return;
            const std::string key = !n->inputs.empty() ? n->inputs[0].value : std::string("Space");
            const std::string when = n->inputs.size() > 1 ? lowerText(trimText(n->inputs[1].value)) : std::string("pulsada");
            const char* fn = when == "soltada" || when == "released" || when == "up"  ? "Input.getKeyUp"
                             : when == "mantenida" || when == "held" || when == "down" ? "Input.getKey"
                                                                                        : "Input.getKeyDown";
            bool down = false;
            try {
                down = rt.native.call(fn, {api::Value(key)}).truthy();
            } catch (const api::Error& e) {
                fault(inst, n->id, e.what());
                return;
            }
            if (down) event(inst, *n, {});
        }
        events(inst, "event.update", {api::Value(dt)});
    }

    void forgetBindings(Instance& inst) {
        for (const long long id : inst.bindings) {
            try {
                rt.native.call("Input.unbindAction", {api::Value(id)});
            } catch (const api::Error&) {
            }
        }
        inst.bindings.clear();
        for (const long long id : inst.ws_listeners) {
            try {
                rt.native.call("WebSocket.off", {api::Value(id)});
            } catch (const api::Error&) {
            }
        }
        inst.ws_listeners.clear();
        std::erase_if(callbacks, [&](const auto& item) { return item.second.first == inst.entity; });
    }

    // OnDestroy: suelta las acciones y los Event Destroy.
    void destroy(Instance& inst) {
        forgetBindings(inst);
        if (inst.started) events(inst, "event.destroy");
    }

    void frame(Phase phase, float dt) {
        if (rt.world == nullptr) return;
        if (phase == Phase::Update) sync();
        for (const entt::entity h : order()) {
            Instance* inst = live(h);
            if (inst == nullptr) continue;
            if (phase == Phase::Update) {
                if (!inst->started) start(*inst);
                if (!inst->failed) update(*inst, dt);
            } else if (inst->started) {
                events(*inst, "event.late_update", {api::Value(dt)});
            }
        }
    }

    void fixed(float step) {
        for (const entt::entity h : order()) {
            Instance* inst = live(h);
            if (inst != nullptr && inst->started) events(*inst, "event.fixed_update", {api::Value(step)});
        }
    }

    void collisions() {
        if (rt.world == nullptr || instances.empty()) return;
        for (const physics::PhysicsEvent& event : rt.frame_events) {
            const char* kind = nullptr;
            switch (event.type) {
                case physics::PhysicsEventType::CollisionEnter: kind = "event.collision_enter"; break;
                case physics::PhysicsEventType::CollisionStay: kind = "event.collision_stay"; break;
                case physics::PhysicsEventType::CollisionExit: kind = "event.collision_exit"; break;
                case physics::PhysicsEventType::TriggerEnter: kind = "event.trigger_enter"; break;
                case physics::PhysicsEventType::TriggerStay: kind = "event.trigger_stay"; break;
                case physics::PhysicsEventType::TriggerExit: kind = "event.trigger_exit"; break;
                default: break;
            }
            if (kind == nullptr) continue;
            const physics::PhysicsEvent sides[2] = {event, event.flipped()};
            for (const physics::PhysicsEvent& side : sides) {
                if (!side.a.valid() || !side.b.valid()) continue;
                Instance* inst = live(side.a.handle());
                if (inst == nullptr || !inst->started) continue;
                api::Value contact = api::Value::object();
                contact.set("point", side.point);
                contact.set("normal", side.normal);
                contact.set("relativeVelocity", side.relative_velocity);
                events(*inst, kind, {rt.entityValue(side.b), contact});
            }
        }
    }

    // Lo que define un nodo para su estado guardado (tipo, funcion y pines).
    static std::string signature(const Node& n) {
        std::string s = n.kind + "|" + n.fn + "|" + (n.disabled ? "off" : "on");
        for (const vscript::Pin& p : n.inputs) s += "|i" + std::to_string(static_cast<int>(p.type)) + p.name;
        for (const vscript::Pin& p : n.outputs) s += "|o" + std::to_string(static_cast<int>(p.type)) + p.name;
        return s;
    }

    // Al recargar: el estado, los valores y las esperas de los nodos que ya no
    // estan o cambiaron (un Delay borrado, un Switch con otras salidas) se
    // descartan; no se puede seguir en un nodo que no es el mismo.
    static void forgetChanged(Instance& inst, const Loaded& before, const Loaded& after) {
        std::set<int> changed;
        for (const Node& old : before.graph.nodes) {
            const Node* now = after.node(old.id);
            if (now == nullptr || signature(*now) != signature(old)) changed.insert(old.id);
        }
        if (changed.empty()) return;
        std::erase_if(inst.state, [&](const auto& item) { return changed.contains(item.first); });
        std::erase_if(inst.slots, [&](const auto& item) { return changed.contains(item.first.first); });
        std::erase_if(inst.pending, [&](const Pending& p) {
            return std::any_of(p.frames.begin(), p.frames.end(), [&](const Frame& f) { return changed.contains(f.node); });
        });
    }

    // Guardar un .crgraph en Play: las instancias siguen con sus datos, con
    // los nodos nuevos y lo que les falte (variables nuevas...).
    bool reload(const std::string& file) {
        if (lowerText(std::filesystem::path(file).extension().string()) != vscript::kGraphExtension) return false;
        failed.erase(file);
        graphs.erase(file);
        if (!rt.running) return true;
        std::string error;
        std::shared_ptr<const Loaded> g = load(file, error);
        if (!g) {
            failed[file] = error;
            rt.fail(file, error);
            return true;
        }
        graphs[file] = g;
        for (auto& [h, inst] : instances) {
            if (inst.graph->file != file) continue;
            forgetChanged(inst, *inst.graph, *g);
            inst.graph = g;
            inst.failed = false;
            init(inst);
        }
        rt.write(0, "Visual Script recargado: " + file);
        return true;
    }

    void clear() {
        graphs.clear();
        failed.clear();
        instances.clear();
        callbacks.clear();
        brk.pending = false;
    }

    // --- VisualScriptDebugHost ---
    static std::string debugText(const api::Value& v) {
        if (v.isNumber()) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%g", v.asNumber());
            return buffer;
        }
        return v.asString();
    }

    bool debug(ecs::Entity entity, ScriptSystem::VisualScriptDebug& out) override {
        out = ScriptSystem::VisualScriptDebug{};
        if (!entity.valid()) return false;
        const auto it = instances.find(entity.handle());
        if (it == instances.end()) return false;
        const Instance& inst = it->second;
        out.graph = inst.graph->file;
        out.failed = inst.failed;
        out.now = rt.time;
        out.executed = inst.executed;
        for (const auto& [key, value] : inst.slots) {
            out.values[std::to_string(key.first) + ":" + std::to_string(key.second)] = debugText(value);
        }
        for (const auto& [name, value] : inst.vars) out.variables[name] = debugText(value);
        return true;
    }
    void setDebugging(bool on) override { debugging = on; }
    void setBreakpoints(const std::string& graph, const std::vector<int>& nodes) override {
        editor_breakpoints[graph] = std::set<int>(nodes.begin(), nodes.end());
    }
    bool takeBreak(std::string& graph, int& node, entt::entity& entity) override {
        if (!brk.pending) return false;
        graph = brk.graph;
        node = brk.node;
        entity = brk.entity;
        brk.pending = false;
        return true;
    }
    std::vector<entt::entity> objects(const std::string& graph) const override {
        std::vector<entt::entity> out;
        for (const auto& [h, inst] : instances) {
            if (graph.empty() || inst.graph->file == graph) out.push_back(h);
        }
        return out;
    }
};

}  // namespace

void registerVisualScriptRuntime(Runtime& rt) {
    const auto host = std::make_shared<Host>(rt);
    rt.vs_debug = host;
    Host* h = host.get();

    // Input Action: el callback local llega aqui (en la fase Input).
    rt.native.setLocalInvoker([h](std::uint64_t id, const api::Value::Array& args) {
        const auto it = h->callbacks.find(id);
        if (it == h->callbacks.end()) return;
        const auto [entity, node] = it->second;
        Instance* inst = h->live(entity);
        if (inst == nullptr) return;
        const std::shared_ptr<const Loaded> g = inst->graph;
        if (const Node* n = g->node(node)) h->event(*inst, *n, {args.empty() ? api::Value{} : args[0]});
    });

    rt.onStart([h] {
        h->clear();
        h->sync();
    });
    rt.onFrame(Phase::Events, [h](float) {
        h->sync();
        h->collisions();
    });
    rt.onFrame(Phase::Update, [h](float dt) { h->frame(Phase::Update, dt); });
    rt.onFrame(Phase::Late, [h](float dt) { h->frame(Phase::Late, dt); });
    rt.onFixed([h](float step) { h->fixed(step); });
    rt.onEvent([h](ecs::Entity target, const std::string& method, const api::Value& value) {
        if (!target.valid()) return;
        if (Instance* inst = h->live(target.handle())) h->customEvent(*inst, method, value);
        // Otra busqueda: el Custom Event pudo destruir el objeto (y su instancia).
        if (method.rfind("OnVideo", 0) == 0) {
            if (Instance* inst = h->live(target.handle())) h->videoEvent(*inst, method, value);
        }
    });
    rt.onDestroying([h](entt::entity e) {
        const auto it = h->instances.find(e);
        if (it == h->instances.end()) return;
        h->destroy(it->second);
        h->instances.erase(e);
    });
    rt.onReload([h](const std::string& file) { return h->reload(file); });
    rt.onStop([h](bool was_running) {
        if (was_running) {
            for (const entt::entity e : h->order()) {
                const auto it = h->instances.find(e);
                if (it != h->instances.end() && !it->second.failed) h->destroy(it->second);
            }
        }
        h->clear();
    });
}

}  // namespace cramion::scripting::native
