// StateMachine y BehaviorTree: las maquinas de estados (ai/StateMachine.h) y
// los Behavior Trees (ai/BehaviorTree.h) de los objetos en Play, y su API.
//
// Fase Ai de cada frame (despues de los Update, como antes): primero las
// maquinas de estados y despues los arboles, cada uno en orden de entidad.
// Los estados ya no tienen codigo Lua: lo que pasa llega a los scripts de C++
// del objeto como mensajes (Script::onMessage):
//   "OnStateEnter"  {machine, state, from}     al entrar (from nil = entrada)
//   "OnStateExit"   {machine, state, to}       al salir
//   "OnStateUpdate" {machine, state, dt, time} cada frame, en los estados con
//                   send_update ("Enviar OnStateUpdate"); time = s en el estado
// Run Script de los arboles: "OnBtTask" {node, function, first} cada tick
// mientras corre (sigue Running hasta BehaviorTree.finishTask(entity, ok) o
// bt.finishTask(ok)); si se corta, "OnBtAbort" {node, function}; el servicio
// Run Script, "OnBtService" {node, function}. Una tarea registrada con
// BehaviorTree.registerTask se llama a ella en lugar del mensaje.
// Las condiciones "Expresion" y Script Condition las evalua ai/Expression.

#include "Modules.h"

#include "CramionCore/ai/BehaviorTree.h"
#include "CramionCore/ai/StateMachine.h"
#include "CramionCore/scripting/CppScripts.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cramion::scripting::native {

namespace {

// --- Handles: la maquina o el arbol de un objeto ---------------------------------

struct MachineHandle final : api::Handle {
    explicit MachineHandle(entt::entity e) : entity(e) {}
    std::string_view typeName() const override { return "StateMachine"; }
    entt::entity entity;
};

struct TreeHandle final : api::Handle {
    explicit TreeHandle(entt::entity e) : entity(e) {}
    std::string_view typeName() const override { return "BehaviorTree"; }
    entt::entity entity;
};

std::uint32_t idOf(entt::entity h) { return static_cast<std::uint32_t>(entt::to_integral(h)); }

// "IA/Enemigo.crfsm" (para los avisos y la recarga).
std::string relativeOf(const Runtime& rt, const std::filesystem::path& file) {
    std::error_code e;
    const std::filesystem::path rel = std::filesystem::relative(file, rt.root, e);
    const std::u8string text = (e ? file.filename() : rel).generic_u8string();
    return std::string(text.begin(), text.end());
}

std::string stemOf(const std::filesystem::path& file) {
    const std::u8string text = file.stem().u8string();
    return std::string(text.begin(), text.end());
}

// Los .crfsm / .crbt de Assets por UUID (se busca como mucho una vez por frame).
struct AssetIndex {
    explicit AssetIndex(const char* ext) : extension(ext) {}
    const char* extension;
    std::unordered_map<Uuid, std::filesystem::path> files;
    std::uint64_t indexed_frame = ~0ull;
    std::set<Uuid> missing;  // sin archivo: no se busca cada frame (hasta recargar)

    void index(const Runtime& rt) {
        if (indexed_frame == rt.frame) return;
        indexed_frame = rt.frame;
        files.clear();
        std::error_code e;
        if (rt.root.empty() || !std::filesystem::is_directory(rt.root, e)) return;
        for (std::filesystem::recursive_directory_iterator it(rt.root, std::filesystem::directory_options::skip_permission_denied, e);
             !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e)) {
            if (it->path().extension() != extension) continue;
            std::ifstream in(it->path(), std::ios::binary);
            const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_object() && j.contains("uuid") && j["uuid"].is_string()) {
                const Uuid uuid = Uuid::parse(j["uuid"].get<std::string>());
                if (uuid.valid()) files[uuid] = it->path();
            }
        }
    }

    // El archivo (vacio si no esta; avisa una vez con `who`).
    std::filesystem::path find(Runtime& rt, const Uuid& uuid, const char* who) {
        if (missing.contains(uuid)) return {};
        auto it = files.find(uuid);
        if (it == files.end()) {
            index(rt);
            it = files.find(uuid);
            if (it == files.end()) {
                missing.insert(uuid);
                rt.write(1, std::string(who) + ": no se encuentra el " + extension + " " + uuid.toString());
                return {};
            }
        }
        return it->second;
    }

    void reset() {
        indexed_frame = ~0ull;
        missing.clear();
    }
    void clear() {
        files.clear();
        reset();
    }
};

// --- Pizarra <-> API -----------------------------------------------------------------

// Objeto por UUID, nombre o tag (las variables entity guardan ese texto).
entt::entity resolveEntity(const Runtime& rt, const std::string& text) {
    if (rt.world == nullptr || text.empty()) return entt::null;
    if (const Uuid uuid = Uuid::parse(text); uuid.valid()) {
        if (const ecs::Entity e = rt.world->find(uuid); e.valid()) return e.handle();
    }
    if (const ecs::Entity e = rt.world->findByName(text); e.valid()) return e.handle();
    if (const ecs::Entity e = rt.world->findWithTag(text); e.valid()) return e.handle();
    return entt::null;
}

void resolveEntities(const Runtime& rt, std::vector<ai::Variable>& vars) {
    for (ai::Variable& v : vars) {
        if (v.value.type != ai::VarType::Entity) continue;
        const entt::entity h = resolveEntity(rt, v.value.s);
        v.value.entity = h == entt::null ? ai::kNoEntity : idOf(h);
    }
}

api::Value toApi(const Runtime& rt, const ai::Value& v) {
    switch (v.type) {
        case ai::VarType::Bool: return api::Value(v.b);
        case ai::VarType::Int: return api::Value(static_cast<double>(std::llround(v.n)));
        case ai::VarType::Float: return api::Value(v.n);
        case ai::VarType::String: return api::Value(v.s);
        case ai::VarType::Vector: return api::Value(v.v);
        case ai::VarType::Entity: {
            if (v.entity == ai::kNoEntity) return {};
            const ecs::Entity e = rt.entity(static_cast<entt::entity>(v.entity));
            return e.valid() ? rt.entityValue(e) : api::Value{};
        }
    }
    return {};
}

const char* kindOf(const api::Value& v) {
    switch (v.type()) {
        case api::Value::Type::Nil: return "nil";
        case api::Value::Type::Bool: return "bool";
        case api::Value::Type::Number: return "numero";
        case api::Value::Type::String: return "texto";
        case api::Value::Type::Vec3: return "Vec3";
        case api::Value::Type::Quat: return "Quat";
        case api::Value::Type::Entity: return "Entity";
        case api::Value::Type::Array: return "lista";
        case api::Value::Type::Object: return "objeto";
        case api::Value::Type::Function: return "funcion";
        case api::Value::Type::Handle: return "objeto del motor";
    }
    return "?";
}

// El valor en la variable (con su tipo). false si no vale para ese tipo.
bool fromApi(const Runtime& rt, const api::Value& o, ai::Value& v) {
    switch (v.type) {
        case ai::VarType::Bool:
            v.b = o.isBool() ? o.truthy() : (o.isNumber() ? o.asNumber() != 0.0 : !o.isNil());
            return true;
        case ai::VarType::Int:
        case ai::VarType::Float:
            if (o.isNumber()) v.n = o.asNumber();
            else if (o.isBool()) v.n = o.truthy() ? 1.0 : 0.0;
            else return false;
            if (v.type == ai::VarType::Int) v.n = static_cast<double>(std::llround(v.n));
            return true;
        case ai::VarType::String:
            if (!o.isString() && !o.isNumber()) return false;
            v.s = o.asString();
            return true;
        case ai::VarType::Vector:
            if (!o.isVec3()) return false;
            v.v = o.asVec3();
            return true;
        case ai::VarType::Entity:
            if (o.isNil() || (o.isEntity() && o.asEntity() == entt::null)) {
                v.entity = ai::kNoEntity;
                v.s.clear();
                return true;
            }
            if (o.isEntity()) {
                const ecs::Entity e = rt.entity(o.asEntity());
                v.entity = e.valid() ? idOf(e.handle()) : ai::kNoEntity;
                v.s = e.valid() ? e.name() : std::string{};
                return true;
            }
            if (o.isString()) {
                v.s = o.asString();
                const entt::entity h = resolveEntity(rt, v.s);
                v.entity = h == entt::null ? ai::kNoEntity : idOf(h);
                return true;
            }
            return false;
    }
    return false;
}

ai::Value* findVar(std::vector<ai::Variable>& vars, const std::string& name) {
    for (ai::Variable& v : vars) {
        if (v.name == name) return &v.value;
    }
    return nullptr;
}

// sm.set / bt.set: cambia la variable o la crea (el tipo sale del valor).
void setVar(Runtime& rt, std::vector<ai::Variable>& vars, const std::string& name, const api::Value& value,
            const char* who, const char* noun) {
    if (ai::Value* v = findVar(vars, name)) {
        if (!fromApi(rt, value, *v)) {
            rt.write(1, std::string(who) + ": " + noun + " \"" + name + "\" es " + ai::varTypeLabel(v->type) +
                            " (no se puede poner un " + kindOf(value) + ")");
        }
        return;
    }
    ai::Variable var;
    var.name = name;
    if (value.isNil()) return;
    if (value.isBool()) var.value.type = ai::VarType::Bool;
    else if (value.isNumber()) var.value.type = ai::VarType::Float;
    else if (value.isString()) var.value.type = ai::VarType::String;
    else if (value.isEntity()) var.value.type = ai::VarType::Entity;
    else if (value.isVec3()) var.value.type = ai::VarType::Vector;
    else {
        rt.write(1, std::string(who) + ": \"" + name + "\" no puede guardar ese valor (bool, numero, texto, entidad o Vec3)");
        return;
    }
    if (fromApi(rt, value, var.value)) vars.push_back(std::move(var));
}

api::Value varsObject(const Runtime& rt, const std::vector<ai::Variable>& vars) {
    api::Value out = api::Value::object();
    for (const ai::Variable& v : vars) out.set(v.name, toApi(rt, v.value));
    return out;
}

// Un mensaje a los scripts de C++ del objeto (onMessage).
void sendMessage(Runtime& rt, entt::entity h, const char* name, const api::Value& value) {
    if (!rt.message_listener) return;
    const ecs::Entity e = rt.entity(h);
    if (e.valid()) rt.message_listener(e, name, rt.native.toJson(value).dump());
}

std::vector<entt::entity> sorted(std::vector<entt::entity> out) {
    std::sort(out.begin(), out.end(), [](entt::entity a, entt::entity b) { return entt::to_integral(a) < entt::to_integral(b); });
    return out;
}

// --- Maquinas de estados ---------------------------------------------------------------

struct FsmHost {
    Runtime& rt;
    explicit FsmHost(Runtime& r) : rt(r) {}

    struct Machine {
        std::filesystem::path path;
        std::string relative;  // "IA/Enemigo.crfsm"
        std::string name;      // "Enemigo" (el `machine` de los mensajes)
        ai::StateMachineAsset data;
    };
    std::unordered_map<Uuid, Machine> machines;
    AssetIndex files{ai::kStateMachineExtension};
    std::unordered_map<entt::entity, Uuid> instances;  // objetos con su pizarra ya puesta
    std::unordered_map<entt::entity, std::shared_ptr<MachineHandle>> handles;

    void problems(const Machine& m) {
        for (const std::string& p : ai::validateStateMachine(m.data)) rt.write(1, "Maquina de estados " + m.relative + ": " + p);
    }

    Machine* machine(const Uuid& uuid) {
        if (const auto it = machines.find(uuid); it != machines.end()) return &it->second;
        const std::filesystem::path file = files.find(rt, uuid, "Maquina de estados");
        if (file.empty()) return nullptr;
        Machine m;
        m.path = file;
        m.relative = relativeOf(rt, file);
        m.name = stemOf(file);
        std::string error;
        if (!ai::loadStateMachine(file, m.data, &error)) {
            rt.write(2, "Maquina de estados: " + error);
            files.missing.insert(uuid);
            return nullptr;
        }
        // Una vez al cargarla: expresiones que no se pueden leer, codigo Lua de antes...
        problems(m);
        return &(machines[uuid] = std::move(m));
    }

    ai::StateMachine* component(entt::entity h) const {
        if (rt.world == nullptr || !rt.world->registry().valid(h)) return nullptr;
        return rt.world->registry().try_get<ai::StateMachine>(h);
    }

    Machine* machineOf(entt::entity h) {
        if (const auto it = instances.find(h); it != instances.end()) return machine(it->second);
        const ai::StateMachine* c = component(h);
        return c != nullptr && c->machine.valid() ? machine(c->machine.uuid) : nullptr;
    }

    api::Value handle(entt::entity h) {
        std::shared_ptr<MachineHandle>& slot = handles[h];
        if (!slot) slot = std::make_shared<MachineHandle>(h);
        return api::Value::handle(slot);
    }

    std::string stateName(const Machine& m, int index) const {
        return index >= 0 && index < static_cast<int>(m.data.states.size()) ? m.data.states[index].name : std::string{};
    }

    // La primera vez (o con otra maquina): pizarra inicial con lo del Inspector.
    void instance(entt::entity h, ai::StateMachine& c, Machine& m) {
        if (const auto it = instances.find(h); it != instances.end() && it->second == c.machine.uuid) return;
        ai::resetRuntime(m.data, c.runtime, c.variables);
        c.runtime.running = c.start_active;
        resolveEntities(rt, c.runtime.vars);
        instances[h] = c.machine.uuid;
    }

    void applyChange(entt::entity h, Machine& m, const ai::StepResult& r) {
        if (!r.changed) return;
        const std::string from = stateName(m, r.from);
        const std::string to = stateName(m, r.to);
        if (r.from >= 0) {
            api::Value exit = api::Value::object();
            exit.set("machine", m.name);
            exit.set("state", from);
            exit.set("to", to);
            sendMessage(rt, h, "OnStateExit", exit);
        }
        if (const ai::StateMachine* c = component(h); c != nullptr && c->debug) {
            const std::string why = r.transition == -2 ? std::string("sm:go")
                                                       : (r.transition >= 0 ? "transicion " + std::to_string(r.transition + 1)
                                                                            : std::string("entrada"));
            rt.write(0, "[Maquina] " + rt.entity(h).name() + ": " + (r.from >= 0 ? from : std::string("Entrada")) + " -> " + to +
                            " (" + why + ")");
        }
        api::Value enter = api::Value::object();
        enter.set("machine", m.name);
        enter.set("state", to);
        enter.set("from", r.from >= 0 ? api::Value(from) : api::Value{});
        sendMessage(rt, h, "OnStateEnter", enter);
    }

    // sm.go pedidos mientras tanto (un script que responde enseguida): se
    // aplican ya (como mucho 8 seguidos).
    void settle(entt::entity h, Machine& m) {
        for (int i = 0; i < 8; ++i) {
            ai::StateMachine* c = component(h);
            if (c == nullptr || c->runtime.requested < 0) break;
            applyChange(h, m, ai::applyRequestedState(m.data, c->runtime));
        }
        if (ai::StateMachine* c = component(h)) c->runtime.requested = -1;
    }

    // Un frame de una maquina: transiciones, OnStateUpdate y los sm.go pedidos.
    void tick(entt::entity h, Machine& m, float dt) {
        ai::StateMachine* c = component(h);
        if (c == nullptr || !c->runtime.running || m.data.states.empty()) return;
        if (!c->runtime.started) {
            const int requested = c->runtime.requested;  // un sm.go antes de empezar: despues de entrar
            applyChange(h, m, ai::startStateMachine(m.data, c->runtime));
            c = component(h);
            if (c == nullptr) return;
            if (requested >= 0 && c->runtime.requested < 0) c->runtime.requested = requested;
            settle(h, m);
        }
        c = component(h);
        if (c == nullptr) return;
        applyChange(h, m, ai::stepStateMachine(m.data, c->runtime, dt));
        settle(h, m);
        c = component(h);
        if (c == nullptr) return;
        const int state = c->runtime.state;
        if (state >= 0 && state < static_cast<int>(m.data.states.size()) && m.data.states[state].send_update) {
            api::Value update = api::Value::object();
            update.set("machine", m.name);
            update.set("state", m.data.states[state].name);
            update.set("dt", dt);
            update.set("time", c->runtime.state_time);
            sendMessage(rt, h, "OnStateUpdate", update);
            settle(h, m);
        }
    }

    void update(ecs::World& world, float dt) {
        for (auto it = instances.begin(); it != instances.end();) {
            const ai::StateMachine* c = component(it->first);
            if (c == nullptr || c->machine.uuid != it->second) it = instances.erase(it);
            else ++it;
        }
        std::vector<entt::entity> order;
        for (const entt::entity h : world.registry().view<ai::StateMachine>()) order.push_back(h);
        for (const entt::entity h : sorted(std::move(order))) {
            if (!world.registry().valid(h) || !world.wrap(h).activeInHierarchy()) continue;
            ai::StateMachine* c = component(h);
            if (c == nullptr || !c->machine.valid()) continue;
            Machine* m = machine(c->machine.uuid);
            if (m == nullptr) continue;
            instance(h, *c, *m);
            tick(h, *m, dt);
        }
    }

    // sm.restart: sale del estado y vuelve a la entrada con la pizarra inicial.
    void restart(entt::entity h) {
        ai::StateMachine* c = component(h);
        if (c == nullptr) return;
        if (Machine* m = machineOf(h)) {
            if (c->runtime.started && c->runtime.state >= 0) {
                api::Value exit = api::Value::object();
                exit.set("machine", m->name);
                exit.set("state", stateName(*m, c->runtime.state));
                exit.set("to", api::Value{});
                sendMessage(rt, h, "OnStateExit", exit);
                c = component(h);
                if (c == nullptr) return;
            }
            ai::resetRuntime(m->data, c->runtime, c->variables);
            resolveEntities(rt, c->runtime.vars);
        }
        c->runtime.running = true;
        c->runtime.started = false;
    }

    // Guardar un .crfsm en Play: se vuelve a leer; cada objeto sigue en su
    // estado con sus variables (las nuevas del asset se anaden).
    bool reload(const std::string& file) {
        if (std::filesystem::path(file).extension() != ai::kStateMachineExtension) return false;
        files.reset();
        for (auto& [uuid, m] : machines) {
            if (m.relative != file) continue;
            ai::StateMachineAsset fresh;
            std::string error;
            if (!ai::loadStateMachine(m.path, fresh, &error)) {
                rt.write(2, "Maquina de estados: " + error);
                continue;
            }
            m.data = std::move(fresh);
            problems(m);
            for (const auto& [h, machine_uuid] : instances) {
                if (machine_uuid != uuid) continue;
                ai::StateMachine* c = component(h);
                if (c == nullptr) continue;
                ai::Runtime& r = c->runtime;
                if (r.state >= static_cast<int>(m.data.states.size())) {
                    r.started = false;  // ese estado ya no existe: empieza otra vez
                    r.state = -1;
                }
                for (const ai::Variable& v : m.data.variables) {
                    if (r.find(v.name) == nullptr) r.vars.push_back(v);
                }
                resolveEntities(rt, r.vars);
            }
            rt.write(0, "Maquina de estados recargada: " + m.relative);
        }
        return true;
    }

    void clear() {
        machines.clear();
        files.clear();
        instances.clear();
        handles.clear();
    }
};

// --- Behavior Trees --------------------------------------------------------------------

struct BtHost {
    Runtime& rt;
    explicit BtHost(Runtime& r) : rt(r) {}

    struct Tree {
        std::filesystem::path path;
        std::string relative;  // "IA/Guardia.crbt"
        ai::BehaviorTreeAsset data;
    };
    std::unordered_map<Uuid, Tree> trees;
    AssetIndex files{ai::kBehaviorTreeExtension};
    std::unordered_map<entt::entity, Uuid> instances;
    std::unordered_map<entt::entity, std::shared_ptr<TreeHandle>> handles;
    // BehaviorTree.registerTask: una funcion o {start, update, abort}.
    struct Task {
        api::Value fn;
        api::Value start;
        api::Value update;
        api::Value abort;
    };
    std::unordered_map<std::string, Task> tasks;
    std::set<std::string> warned;      // tareas que no existen (se avisa una vez)
    std::vector<ai::BtNoise> noises;   // BehaviorTree.reportNoise
    float last_dt = 0.0f;

    void problems(const Tree& t) {
        for (const std::string& p : ai::validateBehaviorTree(t.data)) rt.write(1, "Behavior Tree " + t.relative + ": " + p);
    }

    Tree* tree(const Uuid& uuid) {
        if (const auto it = trees.find(uuid); it != trees.end()) return &it->second;
        const std::filesystem::path file = files.find(rt, uuid, "Behavior Tree");
        if (file.empty()) return nullptr;
        Tree t;
        t.path = file;
        t.relative = relativeOf(rt, file);
        std::string error;
        if (!ai::loadBehaviorTree(file, t.data, &error)) {
            rt.write(2, "Behavior Tree: " + error);
            files.missing.insert(uuid);
            return nullptr;
        }
        problems(t);
        return &(trees[uuid] = std::move(t));
    }

    ai::BehaviorTree* component(entt::entity h) const {
        if (rt.world == nullptr || !rt.world->registry().valid(h)) return nullptr;
        return rt.world->registry().try_get<ai::BehaviorTree>(h);
    }

    Tree* treeOf(entt::entity h) {
        const ai::BehaviorTree* c = component(h);
        return c != nullptr && c->tree.valid() ? tree(c->tree.uuid) : nullptr;
    }

    api::Value handle(entt::entity h) {
        std::shared_ptr<TreeHandle>& slot = handles[h];
        if (!slot) slot = std::make_shared<TreeHandle>(h);
        return api::Value::handle(slot);
    }

    void instance(entt::entity h, ai::BehaviorTree& c, Tree& t) {
        if (const auto it = instances.find(h); it != instances.end() && it->second == c.tree.uuid) return;
        ai::resetBehaviorTree(t.data, c.runtime, c.variables, rt.world);
        c.runtime.running = c.start_active;
        c.runtime.rng.seed(idOf(h) * 2654435761u + 17u);
        c.tick_accumulator = 0.0f;
        instances[h] = c.tree.uuid;
    }

    bool hasScripts(entt::entity h) const {
        return rt.world != nullptr && rt.world->registry().valid(h) && rt.world->registry().all_of<CppScript>(h);
    }

    static api::Value nodeMessage(const Tree& t, int node, const std::string& name) {
        api::Value v = api::Value::object();
        v.set("node", node >= 0 && node < static_cast<int>(t.data.nodes.size()) ? t.data.nodes[node].title() : std::string{});
        v.set("function", name);
        return v;
    }

    // Run Script: la tarea registrada o el mensaje OnBtTask; sigue Running
    // hasta finishTask (Idle = no hay quien la haga).
    ai::BtStatus scriptTask(entt::entity h, const Tree& t, int node, const std::string& name, bool first) {
        if (const auto it = tasks.find(name); it != tasks.end()) {
            const Task& task = it->second;
            const api::Value bt = handle(h);
            if (task.fn.isFunction()) {
                rt.native.invoke(task.fn, {bt, api::Value(first)});
            } else {
                if (first && task.start.isFunction()) rt.native.invoke(task.start, {bt});
                if (task.update.isFunction()) rt.native.invoke(task.update, {bt, api::Value(last_dt)});
            }
            return ai::BtStatus::Running;
        }
        if (hasScripts(h)) {
            api::Value message = nodeMessage(t, node, name);
            message.set("first", first);
            sendMessage(rt, h, "OnBtTask", message);
            return ai::BtStatus::Running;
        }
        if (warned.insert(name).second) {
            rt.write(1, "Behavior Tree: nadie hace la tarea \"" + name +
                            "\" (un script de C++ en el objeto con onMessage(\"OnBtTask\") o BehaviorTree.registerTask)");
        }
        return ai::BtStatus::Idle;
    }

    void scriptAbort(entt::entity h, const Tree& t, int node, const std::string& name) {
        if (const auto it = tasks.find(name); it != tasks.end()) {
            if (it->second.abort.isFunction()) rt.native.invoke(it->second.abort, {handle(h)});
            return;
        }
        if (hasScripts(h)) sendMessage(rt, h, "OnBtAbort", nodeMessage(t, node, name));
    }

    void scriptService(entt::entity h, const Tree& t, int node, const std::string& name) {
        if (const auto it = tasks.find(name); it != tasks.end()) {
            const api::Value& fn = it->second.fn.isFunction() ? it->second.fn : it->second.update;
            if (fn.isFunction()) rt.native.invoke(fn, {handle(h)});
            return;
        }
        if (hasScripts(h)) {
            sendMessage(rt, h, "OnBtService", nodeMessage(t, node, name));
        } else if (warned.insert(name).second) {
            rt.write(1, "Behavior Tree: el servicio llama a \"" + name + "\", que no existe (un script de C++ con onMessage(\"OnBtService\"))");
        }
    }

    // Send Message: como ScriptSystem::callMethod con un texto.
    void message(std::uint32_t target, const std::string& name, const std::string& value) {
        if (rt.world == nullptr || target == ai::kNoEntity || name.empty()) return;
        const ecs::Entity e = rt.entity(static_cast<entt::entity>(target));
        if (!e.valid()) return;
        const api::Value arg(value);
        for (const auto& fn : rt.hooks.event) fn(e, name, arg);
        if (rt.message_listener) rt.message_listener(e, name, rt.native.toJson(arg).dump());
    }

    ai::BtContext context(entt::entity h, const ai::BehaviorTree& c, const Tree& t, float dt) {
        ai::BtContext ctx;
        ctx.world = rt.world;
        ctx.self = idOf(h);
        ctx.navigation = rt.navigation;
        ctx.physics = rt.physics;
        ctx.noises = &noises;
        ctx.now = rt.time;
        ctx.dt = dt;
        ctx.debug = c.debug;
        ctx.script_task = [this, h, &t](int node, const std::string& name, bool first) { return scriptTask(h, t, node, name, first); };
        ctx.script_abort = [this, h, &t](int node, const std::string& name) { scriptAbort(h, t, node, name); };
        ctx.script_service = [this, h, &t](int node, int, const std::string& name) { scriptService(h, t, node, name); };
        ctx.send_message = [this](std::uint32_t target, const std::string& name, const std::string& value) {
            message(target, name, value);
        };
        ctx.log = [this, h](int level, const std::string& text) { rt.write(level, "[" + rt.entity(h).name() + "] " + text); };
        return ctx;
    }

    void update(ecs::World& world, float dt) {
        // Los ruidos viejos (mas de 10 s) ya no los oye nadie.
        std::erase_if(noises, [&](const ai::BtNoise& n) { return rt.time - n.time > 10.0f; });
        for (auto it = instances.begin(); it != instances.end();) {
            const ai::BehaviorTree* c = component(it->first);
            if (c == nullptr || c->tree.uuid != it->second) it = instances.erase(it);
            else ++it;
        }
        std::vector<entt::entity> order;
        for (const entt::entity h : world.registry().view<ai::BehaviorTree>()) order.push_back(h);
        for (const entt::entity h : sorted(std::move(order))) {
            if (!world.registry().valid(h) || !world.wrap(h).activeInHierarchy()) continue;
            ai::BehaviorTree* c = component(h);
            if (c == nullptr || !c->tree.valid()) continue;
            Tree* t = tree(c->tree.uuid);
            if (t == nullptr) continue;
            instance(h, *c, *t);
            float step = dt;
            if (c->tick_interval > 0.0f) {
                c->tick_accumulator += dt;
                if (c->tick_accumulator < c->tick_interval) continue;
                step = c->tick_accumulator;
                c->tick_accumulator = 0.0f;
            }
            last_dt = step;
            ai::BtContext ctx = context(h, *c, *t, step);
            ai::tickBehaviorTree(t->data, c->runtime, ctx);
        }
    }

    void abort(entt::entity h) {
        ai::BehaviorTree* c = component(h);
        const auto it = instances.find(h);
        if (c == nullptr || it == instances.end()) return;
        Tree* t = tree(it->second);
        if (t == nullptr) return;
        ai::BtContext ctx = context(h, *c, *t, 0.0f);
        ai::abortBehaviorTree(t->data, c->runtime, ctx);
    }

    void restart(entt::entity h) {
        if (component(h) == nullptr) return;
        abort(h);
        ai::BehaviorTree* c = component(h);
        if (c == nullptr) return;
        if (Tree* t = treeOf(h)) ai::resetBehaviorTree(t->data, c->runtime, c->variables, rt.world);
        c->runtime.running = true;
    }

    // Guardar un .crbt en Play: se vuelve a leer; cada objeto conserva su
    // pizarra (las claves nuevas se anaden) y su rama empieza de nuevo.
    bool reload(const std::string& file) {
        if (std::filesystem::path(file).extension() != ai::kBehaviorTreeExtension) return false;
        files.reset();
        warned.clear();
        for (auto& [uuid, t] : trees) {
            if (t.relative != file) continue;
            ai::BehaviorTreeAsset fresh;
            std::string error;
            if (!ai::loadBehaviorTree(t.path, fresh, &error)) {
                rt.write(2, "Behavior Tree: " + error);
                continue;
            }
            for (const auto& [h, tree_uuid] : instances) {
                if (tree_uuid != uuid) continue;
                if (ai::BehaviorTree* c = component(h)) {
                    ai::BtContext ctx = context(h, *c, t, 0.0f);
                    ai::abortBehaviorTree(t.data, c->runtime, ctx);
                }
            }
            t.data = std::move(fresh);
            problems(t);
            for (const auto& [h, tree_uuid] : instances) {
                if (tree_uuid != uuid) continue;
                ai::BehaviorTree* c = component(h);
                if (c == nullptr) continue;
                ai::BtRuntime& r = c->runtime;
                for (const ai::Variable& v : t.data.blackboard) {
                    if (r.find(v.name) == nullptr) r.vars.push_back(v);
                }
                r.nodes.assign(t.data.nodes.size(), ai::BtNodeState{});
                r.active_path.clear();
                r.active_task = -1;
            }
            rt.write(0, "Behavior Tree recargado: " + t.relative);
        }
        return true;
    }

    void clear() {
        trees.clear();
        files.clear();
        instances.clear();
        handles.clear();
        tasks.clear();
        warned.clear();
        noises.clear();
    }
};

struct AiState {
    explicit AiState(Runtime& rt) : fsm(rt), bt(rt) {}
    FsmHost fsm;
    BtHost bt;
};

entt::entity machineSelf(const api::Call& c) {
    const std::shared_ptr<MachineHandle> h = c.self().as<MachineHandle>();
    if (!h) throw api::Error("se esperaba un StateMachine");
    return h->entity;
}

entt::entity treeSelf(const api::Call& c) {
    const std::shared_ptr<TreeHandle> h = c.self().as<TreeHandle>();
    if (!h) throw api::Error("se esperaba un BehaviorTree");
    return h->entity;
}

void registerStateMachineApi(Runtime& rt, const std::shared_ptr<AiState>& s) {
    api::NativeApi& api = rt.native;
    const auto comp = [s](const api::Call& c) { return s->fsm.component(machineSelf(c)); };
    const auto prop = [&api](const char* name, api::Function get, api::Doc doc) {
        api.property("StateMachine", name, std::move(get), {}, std::move(doc), true);
    };
    const auto stateName = [s](entt::entity h, int index) -> api::Value {
        const FsmHost::Machine* m = s->fsm.machineOf(h);
        if (m == nullptr || index < 0 || index >= static_cast<int>(m->data.states.size())) return {};
        return api::Value(m->data.states[index].name);
    };
    const auto stateIndex = [s](entt::entity h, const std::string& name) {
        const FsmHost::Machine* m = s->fsm.machineOf(h);
        return m != nullptr ? m->data.findState(name) : -1;
    };

    prop("state", [comp, stateName](api::Call& c) {
        const ai::StateMachine* sm = comp(c);
        return sm != nullptr ? stateName(machineSelf(c), sm->runtime.state) : api::Value{};
    }, {"", "Nombre del estado actual (nil si no ha empezado)", "texto"});
    prop("previous", [comp, stateName](api::Call& c) {
        const ai::StateMachine* sm = comp(c);
        return sm != nullptr ? stateName(machineSelf(c), sm->runtime.previous) : api::Value{};
    }, {"", "Nombre del estado anterior (o nil)", "texto"});
    prop("stateTime", [comp](api::Call& c) {
        const ai::StateMachine* sm = comp(c);
        return api::Value(sm != nullptr ? sm->runtime.state_time : 0.0f);
    }, {"", "Segundos en el estado actual", "numero"});
    prop("time", [comp](api::Call& c) {
        const ai::StateMachine* sm = comp(c);
        return api::Value(sm != nullptr ? sm->runtime.time : 0.0f);
    }, {"", "Segundos desde que empezo la maquina", "numero"});
    prop("changes", [comp](api::Call& c) {
        const ai::StateMachine* sm = comp(c);
        return api::Value(sm != nullptr ? static_cast<double>(sm->runtime.changes) : 0.0);
    }, {"", "Cambios de estado desde que empezo", "numero"});
    prop("running", [comp](api::Call& c) {
        const ai::StateMachine* sm = comp(c);
        return api::Value(sm != nullptr && sm->runtime.running);
    }, {"", "Esta en marcha (no parada con stop)", "bool"});
    prop("entity", [&rt](api::Call& c) {
        const ecs::Entity e = rt.entity(machineSelf(c));
        return e.valid() ? rt.entityValue(e) : api::Value{};
    }, {"", "El objeto de la maquina", "Entity"});
    prop("vars", [&rt, comp](api::Call& c) {
        const ai::StateMachine* sm = comp(c);
        return sm != nullptr ? varsObject(rt, sm->runtime.vars) : api::Value::object();
    }, {"", "Las variables (copia: {nombre = valor}); para cambiarlas, set", "objeto"});

    const auto go = [&rt, comp, stateIndex](api::Call& c) {
        ai::StateMachine* sm = comp(c);
        if (sm == nullptr) return api::Value(false);
        const std::string name = c.string(0);
        const int index = stateIndex(machineSelf(c), name);
        if (index < 0) {
            rt.write(1, "sm:go: no hay un estado \"" + name + "\"");
            return api::Value(false);
        }
        sm->runtime.requested = index;
        return api::Value(true);
    };
    api.method("StateMachine", "go", go, {"\"Estado\"", "Cambia a ese estado (antes que las transiciones)", "bool"});
    api.method("StateMachine", "changeState", go, {"\"Estado\"", "Como go", "bool"});
    api.method("StateMachine", "isIn", [comp, stateIndex](api::Call& c) {
        const ai::StateMachine* sm = comp(c);
        return api::Value(sm != nullptr && sm->runtime.state >= 0 && sm->runtime.state == stateIndex(machineSelf(c), c.string(0)));
    }, {"\"Estado\"", "Esta en ese estado?", "bool"});
    api.method("StateMachine", "trigger", [comp](api::Call& c) {
        if (ai::StateMachine* sm = comp(c)) sm->runtime.triggers.push_back(c.string(0));
        return api::Value{};
    }, {"\"nombre\"", "Activa un trigger (vale hasta que se miran las transiciones)"});
    api.method("StateMachine", "get", [&rt, comp](api::Call& c) {
        const ai::StateMachine* sm = comp(c);
        const ai::Value* v = sm != nullptr ? sm->runtime.find(c.string(0)) : nullptr;
        return v != nullptr ? toApi(rt, *v) : api::Value{};
    }, {"\"variable\"", "Valor de una variable (o nil)", "valor"});
    api.method("StateMachine", "set", [&rt, comp](api::Call& c) {
        if (ai::StateMachine* sm = comp(c)) setVar(rt, sm->runtime.vars, c.string(0), c.arg(1), "Maquina de estados", "la variable");
        return api::Value{};
    }, {"\"variable\", valor", "Cambia una variable (o la crea con el tipo del valor)"});
    api.method("StateMachine", "has", [comp](api::Call& c) {
        const ai::StateMachine* sm = comp(c);
        return api::Value(sm != nullptr && sm->runtime.find(c.string(0)) != nullptr);
    }, {"\"variable\"", "Existe esa variable?", "bool"});
    api.method("StateMachine", "states", [s](api::Call& c) {
        api::Value::Array out;
        if (const FsmHost::Machine* m = s->fsm.machineOf(machineSelf(c))) {
            for (const ai::State& st : m->data.states) out.emplace_back(st.name);
        }
        return api::Value(std::move(out));
    }, {"", "Nombres de los estados", "lista de texto"});
    api.method("StateMachine", "start", [comp](api::Call& c) {
        if (ai::StateMachine* sm = comp(c)) sm->runtime.running = true;
        return api::Value{};
    }, {"", "La pone en marcha (o sigue)"});
    api.method("StateMachine", "stop", [comp](api::Call& c) {
        if (ai::StateMachine* sm = comp(c)) sm->runtime.running = false;
        return api::Value{};
    }, {"", "La para (se queda en su estado)"});
    api.method("StateMachine", "restart", [s](api::Call& c) {
        s->fsm.restart(machineSelf(c));
        return api::Value{};
    }, {"", "Vuelve a la entrada con las variables iniciales"});

    const auto of = [s](const ecs::Entity& e) {
        return e.valid() && e.has<ai::StateMachine>() ? s->fsm.handle(e.handle()) : api::Value{};
    };
    api.function("StateMachine.of", [&rt, of](api::Call& c) { return of(rt.entityArg(c, 0)); },
                 {"entity", "La maquina de estados de un objeto (o nil)", "StateMachine"});
    api.function("StateMachine.broadcast", [&rt](api::Call& c) {
        if (rt.world == nullptr) return api::Value(0);
        const std::string name = c.string(0);
        int count = 0;
        for (const entt::entity h : rt.world->registry().view<ai::StateMachine>()) {
            rt.world->registry().get<ai::StateMachine>(h).runtime.triggers.push_back(name);
            ++count;
        }
        return api::Value(count);
    }, {"\"trigger\"", "Activa un trigger en todas las maquinas", "numero"});
    api.method("Entity", "getStateMachine", [&rt, of](api::Call& c) { return of(rt.selfEntity(c)); },
               {"", "su StateMachine (sm.go/trigger/get/set) o nil", "StateMachine"});
}

void registerBehaviorTreeApi(Runtime& rt, const std::shared_ptr<AiState>& s) {
    api::NativeApi& api = rt.native;
    const auto comp = [s](const api::Call& c) { return s->bt.component(treeSelf(c)); };
    const auto prop = [&api](const char* name, api::Function get, api::Doc doc) {
        api.property("BehaviorTree", name, std::move(get), {}, std::move(doc), true);
    };
    prop("running", [comp](api::Call& c) {
        const ai::BehaviorTree* bt = comp(c);
        return api::Value(bt != nullptr && bt->runtime.running);
    }, {"", "Esta en marcha", "bool"});
    prop("time", [comp](api::Call& c) {
        const ai::BehaviorTree* bt = comp(c);
        return api::Value(bt != nullptr ? bt->runtime.time : 0.0f);
    }, {"", "Segundos desde que empezo", "numero"});
    prop("cycles", [comp](api::Call& c) {
        const ai::BehaviorTree* bt = comp(c);
        return api::Value(bt != nullptr ? static_cast<double>(bt->runtime.cycles) : 0.0);
    }, {"", "Veces que la raiz termino", "numero"});
    prop("activeTask", [s, comp](api::Call& c) {
        const ai::BehaviorTree* bt = comp(c);
        const BtHost::Tree* t = bt != nullptr ? s->bt.treeOf(treeSelf(c)) : nullptr;
        const int n = bt != nullptr ? bt->runtime.active_task : -1;
        if (t == nullptr || n < 0 || n >= static_cast<int>(t->data.nodes.size())) return api::Value{};
        return api::Value(t->data.nodes[n].title());
    }, {"", "Nombre de la tarea que corre (o nil)", "texto"});
    prop("entity", [&rt](api::Call& c) {
        const ecs::Entity e = rt.entity(treeSelf(c));
        return e.valid() ? rt.entityValue(e) : api::Value{};
    }, {"", "El objeto del arbol", "Entity"});
    prop("vars", [&rt, comp](api::Call& c) {
        const ai::BehaviorTree* bt = comp(c);
        return bt != nullptr ? varsObject(rt, bt->runtime.vars) : api::Value::object();
    }, {"", "La pizarra (copia: {clave = valor}); para cambiarla, set", "objeto"});

    api.method("BehaviorTree", "get", [&rt, comp](api::Call& c) {
        const ai::BehaviorTree* bt = comp(c);
        const ai::Value* v = bt != nullptr ? bt->runtime.find(c.string(0)) : nullptr;
        return v != nullptr ? toApi(rt, *v) : api::Value{};
    }, {"\"clave\"", "Valor de una clave de la pizarra (o nil)", "valor"});
    api.method("BehaviorTree", "set", [&rt, comp](api::Call& c) {
        if (ai::BehaviorTree* bt = comp(c)) setVar(rt, bt->runtime.vars, c.string(0), c.arg(1), "Behavior Tree", "la clave");
        return api::Value{};
    }, {"\"clave\", valor", "Cambia una clave (o la crea con el tipo del valor)"});
    api.method("BehaviorTree", "has", [comp](api::Call& c) {
        const ai::BehaviorTree* bt = comp(c);
        return api::Value(bt != nullptr && bt->runtime.find(c.string(0)) != nullptr);
    }, {"\"clave\"", "Existe esa clave?", "bool"});
    api.method("BehaviorTree", "clear", [comp](api::Call& c) {
        ai::BehaviorTree* bt = comp(c);
        if (ai::Value* v = bt != nullptr ? bt->runtime.find(c.string(0)) : nullptr) {
            v->b = false;
            v->n = 0.0;
            v->s.clear();
            v->v = core::Vec3{};
            v->entity = ai::kNoEntity;
        }
        return api::Value{};
    }, {"\"clave\"", "Vacia una clave (false, 0, \"\", sin objeto)"});
    api.method("BehaviorTree", "isActive", [s, comp](api::Call& c) {
        const ai::BehaviorTree* bt = comp(c);
        const BtHost::Tree* t = bt != nullptr ? s->bt.treeOf(treeSelf(c)) : nullptr;
        if (t == nullptr) return api::Value(false);
        const std::string node = c.string(0);
        for (const int n : bt->runtime.active_path) {
            if (n >= 0 && n < static_cast<int>(t->data.nodes.size()) && t->data.nodes[n].title() == node) return api::Value(true);
        }
        return api::Value(false);
    }, {"\"Nodo\"", "Ese nodo esta en la rama que corre?", "bool"});
    api.method("BehaviorTree", "finishTask", [comp](api::Call& c) {
        if (ai::BehaviorTree* bt = comp(c)) bt->runtime.pending_finish = c.boolean(0, true) ? ai::BtStatus::Success : ai::BtStatus::Failure;
        return api::Value{};
    }, {"true", "Termina la tarea Run Script que corre (true = bien, false = fallo)"});
    api.method("BehaviorTree", "start", [comp](api::Call& c) {
        if (ai::BehaviorTree* bt = comp(c)) bt->runtime.running = true;
        return api::Value{};
    }, {"", "Lo pone en marcha"});
    api.method("BehaviorTree", "stop", [s, comp](api::Call& c) {
        ai::BehaviorTree* bt = comp(c);
        if (bt == nullptr) return api::Value{};
        s->bt.abort(treeSelf(c));
        if (ai::BehaviorTree* again = comp(c)) again->runtime.running = false;
        return api::Value{};
    }, {"", "Corta lo que corre y lo para"});
    api.method("BehaviorTree", "restart", [s](api::Call& c) {
        s->bt.restart(treeSelf(c));
        return api::Value{};
    }, {"", "Corta todo y empieza con la pizarra inicial"});

    const auto of = [s](const ecs::Entity& e) {
        return e.valid() && e.has<ai::BehaviorTree>() ? s->bt.handle(e.handle()) : api::Value{};
    };
    api.function("BehaviorTree.of", [&rt, of](api::Call& c) { return of(rt.entityArg(c, 0)); },
                 {"entity", "el arbol de un objeto (o nil)", "BehaviorTree"});
    api.function("BehaviorTree.finishTask", [&rt, s](api::Call& c) {
        const ecs::Entity e = rt.entityArg(c, 0);
        if (ai::BehaviorTree* bt = e.valid() ? s->bt.component(e.handle()) : nullptr) {
            bt->runtime.pending_finish = c.boolean(1, true) ? ai::BtStatus::Success : ai::BtStatus::Failure;
        }
        return api::Value{};
    }, {"entity, true", "Termina la tarea Run Script del objeto (true = bien, false = fallo)"});
    api.function("BehaviorTree.registerTask", [&rt, s](api::Call& c) {
        const std::string name = c.string(0);
        const api::Value& task = c.arg(1);
        if (name.empty()) return api::Value(false);
        BtHost::Task t;
        if (task.isFunction()) {
            t.fn = task;
        } else if (task.isObject()) {
            t.start = task["start"];
            t.update = task["update"];
            t.abort = task["abort"];
        }
        if (!t.fn.isFunction() && !t.start.isFunction() && !t.update.isFunction() && !t.abort.isFunction()) {
            rt.write(1, "BehaviorTree.registerTask(\"" + name + "\"): hace falta una funcion o una tabla {start, update, abort}");
            return api::Value(false);
        }
        s->bt.tasks[name] = std::move(t);
        s->bt.warned.erase(name);
        return api::Value(true);
    }, {"\"Atacar\", funcion(bt, primera) o {start, update, abort}",
        "tarea Run Script: se llama cada tick mientras corre; termina con bt.finishTask(true/false)", "bool"});
    api.function("BehaviorTree.reportNoise", [&rt, s](api::Call& c) {
        ai::BtNoise noise;
        noise.position = c.vec3(0);
        noise.radius = static_cast<float>(c.number(1, 10.0));
        noise.time = rt.time;
        if (const ecs::Entity who = rt.entity(c.entity(2)); who.valid()) noise.instigator = idOf(who.handle());
        noise.tag = c.string(3, "");
        std::vector<ai::BtNoise>& noises = s->bt.noises;
        noises.push_back(std::move(noise));
        if (noises.size() > 256) noises.erase(noises.begin());
        return api::Value{};
    }, {"posicion, radio, quien", "ruido que oye el servicio Hearing"});
    api.function("BehaviorTree.broadcast", [&rt, s](api::Call& c) {
        if (rt.world == nullptr) return api::Value(0);
        const std::string key = c.string(0);
        int count = 0;
        std::vector<entt::entity> order;
        for (const entt::entity h : rt.world->registry().view<ai::BehaviorTree>()) order.push_back(h);
        for (const entt::entity h : order) {
            ai::BehaviorTree* bt = s->bt.component(h);
            if (bt == nullptr || bt->runtime.find(key) == nullptr) continue;
            setVar(rt, bt->runtime.vars, key, c.arg(1), "Behavior Tree", "la clave");
            ++count;
        }
        return api::Value(count);
    }, {"\"clave\", valor", "cambia una clave en todos los arboles", "numero"});
    api.method("Entity", "getBehaviorTree", [&rt, of](api::Call& c) { return of(rt.selfEntity(c)); },
               {"", "su BehaviorTree (bt:get/set/start/stop/finishTask) o nil", "BehaviorTree"});
}

}  // namespace

void registerAiApi(Runtime& rt) {
    const auto s = std::make_shared<AiState>(rt);
    registerStateMachineApi(rt, s);
    registerBehaviorTreeApi(rt, s);

    // Despues de los Update (ven los triggers y los set del frame): las
    // maquinas y despues los arboles.
    rt.onFrame(Phase::Ai, [&rt, s](float dt) {
        if (rt.world == nullptr) return;
        s->fsm.update(*rt.world, dt);
        s->bt.update(*rt.world, dt);
    });
    rt.onReload([s](const std::string& file) { return s->fsm.reload(file) || s->bt.reload(file); });
    rt.onStart([s] {
        s->fsm.clear();
        s->bt.clear();
    });
    rt.onStop([&rt, s](bool was_running) {
        // Los NavAgent que movia un arbol se paran.
        if (was_running && rt.world != nullptr) {
            std::vector<entt::entity> trees;
            for (const entt::entity h : rt.world->registry().view<ai::BehaviorTree>()) trees.push_back(h);
            for (const entt::entity h : trees) s->bt.abort(h);
        }
        s->fsm.clear();
        s->bt.clear();  // tambien las tareas registradas (callbacks de los scripts)
    });
}

}  // namespace cramion::scripting::native
