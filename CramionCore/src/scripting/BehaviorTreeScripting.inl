// Behavior Trees en Lua (ai/BehaviorTree.h). Se incluye en Scripting.cpp
// despues de ScriptSystem::Impl: usa su estado de Lua, sus entidades
// (LuaEntity), la fisica, la navegacion y sus mensajes de error.
//
// Cada objeto con el componente BehaviorTree tiene un `self` (una tabla) con
// self.entity, self.bt y self.vars (la pizarra). Las tareas de script:
//   BehaviorTree.registerTask("Atacar", function(self, bt, first) ... end)
//     devuelve "success" / "failure" / "running" (o true / false; nil = bien)
//   BehaviorTree.registerTask("Huir", {start = fn, update = fn, abort = fn})
// Si no hay tarea registrada con ese nombre se busca un metodo del script
// Lua del objeto y despues una funcion global. Si tampoco, y el objeto tiene
// un script de C++, le llega el mensaje (Script::onMessage) y la tarea sigue
// corriendo hasta que llame a bt.finishTask(true/false).

namespace {
// bt en Lua: el arbol de un objeto.
struct LuaBehaviorTree {
    entt::entity handle = entt::null;
};
}  // namespace

struct ScriptSystem::Impl::BtHost {
    ScriptSystem::Impl& d;
    explicit BtHost(ScriptSystem::Impl& impl) : d(impl) {}

    struct Tree {
        std::filesystem::path path;
        std::string relative;  // "IA/Guardia.crbt"
        ai::BehaviorTreeAsset data;
        std::map<std::pair<int, int>, sol::protected_function> conditions;  // (nodo, decorador)
        std::set<std::pair<int, int>> bad_conditions;
    };
    std::unordered_map<Uuid, Tree> trees;
    std::unordered_map<Uuid, std::filesystem::path> files;
    std::uint64_t indexed_frame = ~0ull;
    std::set<Uuid> missing;

    struct Instance {
        Uuid tree{};
        sol::table self;
        sol::table vars;
        sol::environment cond_env;
    };
    std::unordered_map<entt::entity, Instance> instances;
    std::unordered_map<std::string, sol::object> tasks;  // BehaviorTree.registerTask
    std::set<std::string> warned;                         // tareas que no existen (se avisa una vez)
    std::vector<ai::BtNoise> noises;                      // BehaviorTree.reportNoise
    std::function<void(ecs::Entity, const std::string&, const std::string&)> message_listener;

    // --- Assets ---
    std::string relativeOf(const std::filesystem::path& file) const {
        std::error_code e;
        const std::filesystem::path rel = std::filesystem::relative(file, d.root, e);
        const std::u8string text = (e ? file.filename() : rel).generic_u8string();
        return std::string(text.begin(), text.end());
    }

    void indexFiles() {
        if (indexed_frame == d.frame) return;
        indexed_frame = d.frame;
        files.clear();
        std::error_code e;
        if (d.root.empty() || !std::filesystem::is_directory(d.root, e)) return;
        for (std::filesystem::recursive_directory_iterator it(d.root, std::filesystem::directory_options::skip_permission_denied, e);
             !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e)) {
            if (it->path().extension() != ai::kBehaviorTreeExtension) continue;
            std::ifstream in(it->path(), std::ios::binary);
            const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_object() && j.contains("uuid") && j["uuid"].is_string()) {
                const Uuid uuid = Uuid::parse(j["uuid"].get<std::string>());
                if (uuid.valid()) files[uuid] = it->path();
            }
        }
    }

    Tree* tree(const Uuid& uuid) {
        if (const auto it = trees.find(uuid); it != trees.end()) return &it->second;
        if (missing.contains(uuid)) return nullptr;
        auto file = files.find(uuid);
        if (file == files.end()) {
            indexFiles();
            file = files.find(uuid);
            if (file == files.end()) {
                missing.insert(uuid);
                d.write(1, "Behavior Tree: no se encuentra el .crbt " + uuid.toString());
                return nullptr;
            }
        }
        Tree t;
        t.path = file->second;
        t.relative = relativeOf(t.path);
        std::string error;
        if (!ai::loadBehaviorTree(t.path, t.data, &error)) {
            d.write(2, "Behavior Tree: " + error);
            return nullptr;
        }
        for (const std::string& problem : ai::validateBehaviorTree(t.data)) {
            d.write(1, "Behavior Tree " + t.relative + ": " + problem);
        }
        return &(trees[uuid] = std::move(t));
    }

    // --- Pizarra <-> Lua (como las maquinas de estados) ---
    entt::entity resolveEntity(const std::string& text) const {
        if (d.world == nullptr || text.empty()) return entt::null;
        const std::uint32_t h = ai::btResolveEntity(*d.world, text);
        return h == ai::kNoEntity ? entt::null : static_cast<entt::entity>(h);
    }

    sol::object toLua(const ai::Value& v) {
        sol::state& L = *d.lua;
        switch (v.type) {
            case ai::VarType::Bool: return sol::make_object(L, v.b);
            case ai::VarType::Int: return sol::make_object(L, static_cast<lua_Integer>(std::llround(v.n)));
            case ai::VarType::Float: return sol::make_object(L, v.n);
            case ai::VarType::String: return sol::make_object(L, v.s);
            case ai::VarType::Vector: return sol::make_object(L, v.v);
            case ai::VarType::Entity: {
                const entt::entity h = static_cast<entt::entity>(v.entity);
                if (v.entity == ai::kNoEntity || d.world == nullptr || !d.world->registry().valid(h)) return sol::lua_nil;
                return sol::make_object(L, LuaEntity{h, d.world});
            }
        }
        return sol::lua_nil;
    }

    bool fromLua(const sol::object& o, ai::Value& v) {
        const sol::type t = o.get_type();
        switch (v.type) {
            case ai::VarType::Bool:
                v.b = t == sol::type::boolean ? o.as<bool>() : (t == sol::type::number ? o.as<double>() != 0.0 : t != sol::type::lua_nil);
                return true;
            case ai::VarType::Int:
            case ai::VarType::Float:
                if (t == sol::type::number) v.n = o.as<double>();
                else if (t == sol::type::boolean) v.n = o.as<bool>() ? 1.0 : 0.0;
                else return false;
                if (v.type == ai::VarType::Int) v.n = static_cast<double>(std::llround(v.n));
                return true;
            case ai::VarType::String:
                if (t == sol::type::string || t == sol::type::number) {
                    sol::protected_function tostring = (*d.lua)["tostring"];
                    sol::protected_function_result r = tostring(o);
                    v.s = r.valid() ? r.get<std::string>() : std::string{};
                    return true;
                }
                return false;
            case ai::VarType::Vector:
                if (!o.is<Vec3>()) return false;
                v.v = o.as<Vec3>();
                return true;
            case ai::VarType::Entity:
                if (t == sol::type::lua_nil) {
                    v.entity = ai::kNoEntity;
                    v.s.clear();
                    return true;
                }
                if (o.is<LuaEntity>()) {
                    const LuaEntity e = o.as<LuaEntity>();
                    v.entity = e.valid() ? static_cast<std::uint32_t>(entt::to_integral(e.handle)) : ai::kNoEntity;
                    v.s = e.valid() ? e.get().name() : std::string{};
                    return true;
                }
                if (t == sol::type::string) {
                    v.s = o.as<std::string>();
                    const entt::entity h = resolveEntity(v.s);
                    v.entity = h == entt::null ? ai::kNoEntity : static_cast<std::uint32_t>(entt::to_integral(h));
                    return true;
                }
                return false;
        }
        return false;
    }

    ai::BehaviorTree* component(entt::entity h) const {
        if (d.world == nullptr || !d.world->registry().valid(h)) return nullptr;
        return d.world->registry().try_get<ai::BehaviorTree>(h);
    }

    Tree* treeOf(entt::entity h) {
        const ai::BehaviorTree* c = component(h);
        return c != nullptr && c->tree.valid() ? tree(c->tree.uuid) : nullptr;
    }

    sol::object get(entt::entity h, const std::string& name) {
        const ai::BehaviorTree* c = component(h);
        if (c == nullptr) return sol::lua_nil;
        const ai::Value* v = c->runtime.find(name);
        return v != nullptr ? toLua(*v) : sol::object(sol::lua_nil);
    }

    void set(entt::entity h, const std::string& name, const sol::object& value) {
        ai::BehaviorTree* c = component(h);
        if (c == nullptr) return;
        if (ai::Value* v = c->runtime.find(name)) {
            if (!fromLua(value, *v)) {
                d.write(1, "Behavior Tree: la clave \"" + name + "\" es " + ai::varTypeLabel(v->type) + " (no se puede poner un " +
                               std::string(sol::type_name(d.lua->lua_state(), value.get_type())) + ")");
            }
            return;
        }
        ai::Variable var;
        var.name = name;
        const sol::type t = value.get_type();
        if (t == sol::type::lua_nil) return;
        if (t == sol::type::boolean) var.value.type = ai::VarType::Bool;
        else if (t == sol::type::number) var.value.type = ai::VarType::Float;
        else if (t == sol::type::string) var.value.type = ai::VarType::String;
        else if (value.is<LuaEntity>()) var.value.type = ai::VarType::Entity;
        else if (value.is<Vec3>()) var.value.type = ai::VarType::Vector;
        else {
            d.write(1, "Behavior Tree: \"" + name + "\" no puede guardar ese valor (bool, numero, texto, entidad o Vec3)");
            return;
        }
        if (fromLua(value, var.value)) c->runtime.vars.push_back(std::move(var));
    }

    // --- Instancias ---
    Instance& instance(entt::entity h, ai::BehaviorTree& c, Tree& t) {
        if (auto it = instances.find(h); it != instances.end() && it->second.tree == c.tree.uuid) return it->second;
        sol::state& L = *d.lua;
        Instance inst;
        inst.tree = c.tree.uuid;
        inst.self = L.create_table();
        inst.self["entity"] = LuaEntity{h, d.world};
        inst.self["bt"] = LuaBehaviorTree{h};
        inst.vars = L.create_table();
        sol::table vars_meta = L.create_table();
        vars_meta["__index"] = [this, h](const sol::table&, const sol::object& key) -> sol::object {
            return key.get_type() == sol::type::string ? get(h, key.as<std::string>()) : sol::object(sol::lua_nil);
        };
        vars_meta["__newindex"] = [this, h](const sol::table&, const sol::object& key, const sol::object& value) {
            if (key.get_type() == sol::type::string) set(h, key.as<std::string>(), value);
        };
        inst.vars[sol::metatable_key] = vars_meta;
        inst.self["vars"] = inst.vars;
        // Expresiones de Script Condition: las claves por su nombre, self, entity, bt y las globales.
        inst.cond_env = sol::environment(L.lua_state(), sol::create);
        sol::table env_meta = L.create_table();
        const sol::table self = inst.self;
        env_meta["__index"] = [this, h, self](const sol::table&, const sol::object& key, sol::this_state s) -> sol::object {
            if (key.get_type() != sol::type::string) return sol::lua_nil;
            const std::string k = key.as<std::string>();
            if (const ai::BehaviorTree* bc = component(h); bc != nullptr && bc->runtime.find(k) != nullptr) return get(h, k);
            sol::table tt = self;
            if (k == "self") return tt;
            if (k == "entity" || k == "bt" || k == "vars") return tt.raw_get<sol::object>(k);
            return sol::state_view(s).globals().get<sol::object>(k);
        };
        inst.cond_env[sol::metatable_key] = env_meta;
        ai::resetBehaviorTree(t.data, c.runtime, c.variables, d.world);
        c.runtime.running = c.start_active;
        c.runtime.rng.seed(static_cast<std::uint32_t>(entt::to_integral(h)) * 2654435761u + 17u);
        c.tick_accumulator = 0.0f;
        instances[h] = std::move(inst);
        return instances[h];
    }

    // --- Tareas, condiciones y servicios de script ---
    // "success" / "failure" / "running", true / false, nil = bien.
    static ai::BtStatus statusOf(const sol::object& o) {
        switch (o.get_type()) {
            case sol::type::lua_nil:
            case sol::type::none: return ai::BtStatus::Success;
            case sol::type::boolean: return o.as<bool>() ? ai::BtStatus::Success : ai::BtStatus::Failure;
            case sol::type::string: {
                std::string s = o.as<std::string>();
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (s == "running" || s == "corriendo") return ai::BtStatus::Running;
                if (s == "failure" || s == "failed" || s == "fail" || s == "fallo") return ai::BtStatus::Failure;
                return ai::BtStatus::Success;
            }
            default: return ai::BtStatus::Success;
        }
    }

    // Llama a fn(receptor, ...) y devuelve su primer resultado (o nullopt si fallo).
    template <typename... Args>
    std::optional<sol::object> invoke(const sol::object& fn, const std::string& what, Args&&... args) {
        if (fn.get_type() != sol::type::function) return std::nullopt;
        sol::protected_function pf = fn;
        sol::protected_function_result r = pf(std::forward<Args>(args)...);
        if (!r.valid()) {
            sol::error e = r;
            d.write(2, "Behavior Tree (" + what + "): " + e.what());
            return std::nullopt;
        }
        if (r.return_count() == 0) return sol::object(sol::lua_nil);
        sol::object first = r;
        return first;
    }

    ai::BtStatus scriptTask(entt::entity h, Instance& inst, const std::string& name, bool first) {
        const LuaBehaviorTree bt{h};
        // 1. Registrada con BehaviorTree.registerTask.
        if (const auto it = tasks.find(name); it != tasks.end()) {
            const sol::object& task = it->second;
            if (task.get_type() == sol::type::function) {
                const auto r = invoke(task, name, inst.self, bt, first);
                return r ? statusOf(*r) : ai::BtStatus::Failure;
            }
            if (task.is<sol::table>()) {
                sol::table table = task.as<sol::table>();
                if (first) {
                    const sol::object start = table.raw_get<sol::object>("start");
                    if (start.get_type() == sol::type::function) {
                        const auto r = invoke(start, name + ".start", inst.self, bt);
                        if (!r) return ai::BtStatus::Failure;
                        // start puede terminar ya (true/false/"success"); nil = sigue con update.
                        if (r->get_type() != sol::type::lua_nil) {
                            const ai::BtStatus s = statusOf(*r);
                            if (s != ai::BtStatus::Running) return s;
                        }
                    }
                }
                const sol::object update = table.raw_get<sol::object>("update");
                if (update.get_type() != sol::type::function) return ai::BtStatus::Success;
                const auto r = invoke(update, name + ".update", inst.self, bt, static_cast<float>(d.world != nullptr ? lastDt : 0.0f));
                return r ? statusOf(*r) : ai::BtStatus::Failure;
            }
        }
        // 2. Metodo del script Lua del objeto.
        if (const auto it = d.instances.find(h); it != d.instances.end() && !it->second.failed) {
            const sol::object fn = it->second.self[name];
            if (fn.get_type() == sol::type::function) {
                const auto r = invoke(fn, it->second.file + ":" + name, it->second.self, bt, first);
                return r ? statusOf(*r) : ai::BtStatus::Failure;
            }
        }
        // 3. Funcion global.
        const sol::object global = d.lua->globals().get<sol::object>(name);
        if (global.get_type() == sol::type::function) {
            const auto r = invoke(global, name, inst.self, bt, first);
            return r ? statusOf(*r) : ai::BtStatus::Failure;
        }
        // 4. Script de C++ del objeto: el mensaje y espera a bt.finishTask().
        if (d.world != nullptr && d.world->registry().all_of<CppScript>(h)) {
            if (first && message_listener) message_listener(d.world->wrap(h), name, "null");
            return ai::BtStatus::Running;
        }
        if (warned.insert(name).second) {
            d.write(1, "Behavior Tree: no existe la tarea \"" + name + "\" (BehaviorTree.registerTask, un metodo del script o una funcion global)");
        }
        return ai::BtStatus::Idle;
    }

    void scriptAbort(entt::entity h, Instance& inst, const std::string& name) {
        const auto it = tasks.find(name);
        if (it == tasks.end() || !it->second.is<sol::table>()) return;
        const sol::object abort = it->second.as<sol::table>().raw_get<sol::object>("abort");
        if (abort.get_type() == sol::type::function) invoke(abort, name + ".abort", inst.self, LuaBehaviorTree{h});
    }

    bool scriptCondition(Tree& t, Instance& inst, int node, int decorator, const std::string& expression) {
        const std::pair<int, int> key{node, decorator};
        if (t.bad_conditions.contains(key) || expression.empty()) return false;
        auto it = t.conditions.find(key);
        if (it == t.conditions.end()) {
            const std::string name = t.relative + "#" + t.data.nodes[node].title();
            sol::load_result chunk = d.lua->load("return (" + expression + ")", "@" + name);
            if (!chunk.valid()) {
                sol::error e = chunk;
                t.bad_conditions.insert(key);
                d.write(2, "Behavior Tree " + t.relative + " (" + t.data.nodes[node].title() + "): " + e.what());
                return false;
            }
            it = t.conditions.emplace(key, sol::protected_function(chunk)).first;
        }
        inst.cond_env.set_on(it->second);
        sol::protected_function_result r = it->second();
        if (!r.valid()) {
            sol::error e = r;
            t.bad_conditions.insert(key);
            d.write(2, "Behavior Tree " + t.relative + " (" + t.data.nodes[node].title() + "): " + e.what());
            return false;
        }
        const sol::object value = r;
        return value.valid() && value.get_type() != sol::type::lua_nil && !(value.get_type() == sol::type::boolean && !value.as<bool>());
    }

    void scriptService(entt::entity h, Instance& inst, const std::string& name) {
        const LuaBehaviorTree bt{h};
        sol::object fn = sol::lua_nil;
        if (const auto it = tasks.find(name); it != tasks.end()) {
            fn = it->second.is<sol::table>() ? it->second.as<sol::table>().raw_get<sol::object>("update") : it->second;
        }
        if (fn.get_type() != sol::type::function) {
            if (const auto it = d.instances.find(h); it != d.instances.end() && !it->second.failed) {
                const sol::object method = it->second.self[name];
                if (method.get_type() == sol::type::function) {
                    invoke(method, it->second.file + ":" + name, it->second.self, bt);
                    return;
                }
            }
            fn = d.lua->globals().get<sol::object>(name);
        }
        if (fn.get_type() == sol::type::function) {
            invoke(fn, name, inst.self, bt);
        } else if (warned.insert(name).second) {
            d.write(1, "Behavior Tree: el servicio llama a \"" + name + "\", que no existe");
        }
    }

    void sendMessage(std::uint32_t target, const std::string& message, const std::string& value) {
        if (d.world == nullptr || target == ai::kNoEntity || message.empty()) return;
        const entt::entity h = static_cast<entt::entity>(target);
        if (!d.world->registry().valid(h)) return;
        if (const auto it = d.instances.find(h); it != d.instances.end()) d.call(it->second, message.c_str(), value);
        if (message_listener) message_listener(d.world->wrap(h), message, nlohmann::json(value).dump());
    }

    float lastDt = 0.0f;

    ai::BtContext context(entt::entity h, ai::BehaviorTree& c, Tree& t, Instance& inst, float dt) {
        ai::BtContext ctx;
        ctx.world = d.world;
        ctx.self = static_cast<std::uint32_t>(entt::to_integral(h));
        ctx.navigation = d.navigation;
        ctx.physics = d.physics;
        ctx.noises = &noises;
        ctx.now = d.time;
        ctx.dt = dt;
        ctx.debug = c.debug;
        ctx.script_task = [this, h, &inst](int, const std::string& name, bool first) { return scriptTask(h, inst, name, first); };
        ctx.script_abort = [this, h, &inst](int, const std::string& name) { scriptAbort(h, inst, name); };
        ctx.script_condition = [this, &t, &inst](int node, int decorator, const std::string& expression) {
            return scriptCondition(t, inst, node, decorator, expression);
        };
        ctx.script_service = [this, h, &inst](int, int, const std::string& name) { scriptService(h, inst, name); };
        ctx.send_message = [this](std::uint32_t target, const std::string& message, const std::string& value) {
            sendMessage(target, message, value);
        };
        ctx.log = [this, h](int level, const std::string& text) {
            const ecs::Entity e = d.world->wrap(h);
            d.write(level, "[" + e.name() + "] " + text);
        };
        return ctx;
    }

    std::vector<entt::entity> ordered(ecs::World& world) const {
        std::vector<entt::entity> out;
        for (const entt::entity h : world.registry().view<ai::BehaviorTree>()) out.push_back(h);
        std::sort(out.begin(), out.end(), [](entt::entity a, entt::entity b) { return entt::to_integral(a) < entt::to_integral(b); });
        return out;
    }

    void update(ecs::World& world, float dt) {
        // Los ruidos viejos (mas de 10 s) ya no los oye nadie.
        std::erase_if(noises, [&](const ai::BtNoise& n) { return d.time - n.time > 10.0f; });
        for (auto it = instances.begin(); it != instances.end();) {
            const ai::BehaviorTree* c = component(it->first);
            if (c == nullptr || c->tree.uuid != it->second.tree) it = instances.erase(it);
            else ++it;
        }
        for (const entt::entity h : ordered(world)) {
            const ecs::Entity e = world.wrap(h);
            if (!e.activeInHierarchy()) continue;
            ai::BehaviorTree* c = component(h);
            if (c == nullptr || !c->tree.valid()) continue;
            Tree* t = tree(c->tree.uuid);
            if (t == nullptr) continue;
            Instance& inst = instance(h, *c, *t);
            float step = dt;
            if (c->tick_interval > 0.0f) {
                c->tick_accumulator += dt;
                if (c->tick_accumulator < c->tick_interval) continue;
                step = c->tick_accumulator;
                c->tick_accumulator = 0.0f;
            }
            lastDt = step;
            ai::BtContext ctx = context(h, *c, *t, inst, step);
            ai::tickBehaviorTree(t->data, c->runtime, ctx);
        }
    }

    void abort(entt::entity h) {
        ai::BehaviorTree* c = component(h);
        const auto it = instances.find(h);
        if (c == nullptr || it == instances.end()) return;
        Tree* t = tree(it->second.tree);
        if (t == nullptr) return;
        ai::BtContext ctx = context(h, *c, *t, it->second, 0.0f);
        ai::abortBehaviorTree(t->data, c->runtime, ctx);
    }

    void restart(entt::entity h) {
        ai::BehaviorTree* c = component(h);
        if (c == nullptr) return;
        abort(h);
        if (Tree* t = treeOf(h)) ai::resetBehaviorTree(t->data, c->runtime, c->variables, d.world);
        c->runtime.running = true;
    }

    // Guardar un .crbt en Play: se vuelve a leer; cada objeto conserva su
    // pizarra (las claves nuevas se anaden) y su rama empieza de nuevo.
    void reload(const std::string& file) {
        if (std::filesystem::path(file).extension() != ai::kBehaviorTreeExtension) return;
        indexed_frame = ~0ull;
        missing.clear();
        warned.clear();
        for (auto& [uuid, t] : trees) {
            if (t.relative != file) continue;
            ai::BehaviorTreeAsset fresh;
            std::string error;
            if (!ai::loadBehaviorTree(t.path, fresh, &error)) {
                d.write(2, "Behavior Tree: " + error);
                continue;
            }
            for (auto& [h, inst] : instances) {
                if (inst.tree != uuid) continue;
                ai::BehaviorTree* c = component(h);
                if (c == nullptr) continue;
                ai::BtContext ctx = context(h, *c, t, inst, 0.0f);
                ai::abortBehaviorTree(t.data, c->runtime, ctx);
            }
            t.data = std::move(fresh);
            t.conditions.clear();
            t.bad_conditions.clear();
            for (auto& [h, inst] : instances) {
                if (inst.tree != uuid) continue;
                ai::BehaviorTree* c = component(h);
                if (c == nullptr) continue;
                ai::BtRuntime& rt = c->runtime;
                for (const ai::Variable& v : t.data.blackboard) {
                    if (rt.find(v.name) == nullptr) rt.vars.push_back(v);
                }
                rt.nodes.assign(t.data.nodes.size(), ai::BtNodeState{});
                rt.active_path.clear();
                rt.active_task = -1;
            }
            d.write(0, "Behavior Tree recargado: " + t.relative);
        }
    }
};

void ScriptSystem::Impl::bindBehaviorTree(sol::state& L, sol::usertype<LuaEntity>& entity) {
    const auto comp = [this](const LuaBehaviorTree& bt) -> ai::BehaviorTree* {
        if (world == nullptr || !world->registry().valid(bt.handle)) return nullptr;
        return world->registry().try_get<ai::BehaviorTree>(bt.handle);
    };
    const auto nodeName = [this](const LuaBehaviorTree& bt, int index) -> sol::object {
        BtHost::Tree* t = behavior ? behavior->treeOf(bt.handle) : nullptr;
        if (t == nullptr || index < 0 || index >= static_cast<int>(t->data.nodes.size())) return sol::lua_nil;
        return sol::make_object(*lua, t->data.nodes[index].title());
    };
    auto type = L.new_usertype<LuaBehaviorTree>(
        "BehaviorTree", sol::no_constructor,
        "running", sol::property([comp](const LuaBehaviorTree& bt) {
            const ai::BehaviorTree* c = comp(bt);
            return c != nullptr && c->runtime.running;
        }),
        "time", sol::property([comp](const LuaBehaviorTree& bt) {
            const ai::BehaviorTree* c = comp(bt);
            return c != nullptr ? c->runtime.time : 0.0f;
        }),
        "cycles", sol::property([comp](const LuaBehaviorTree& bt) {
            const ai::BehaviorTree* c = comp(bt);
            return c != nullptr ? static_cast<double>(c->runtime.cycles) : 0.0;
        }),
        "activeTask", sol::property([comp, nodeName](const LuaBehaviorTree& bt) -> sol::object {
            const ai::BehaviorTree* c = comp(bt);
            return c != nullptr ? nodeName(bt, c->runtime.active_task) : sol::object(sol::lua_nil);
        }),
        "entity", sol::property([this](const LuaBehaviorTree& bt) { return LuaEntity{bt.handle, world}; }),
        "vars", sol::property([this](const LuaBehaviorTree& bt) -> sol::object {
            if (!behavior) return sol::lua_nil;
            const auto it = behavior->instances.find(bt.handle);
            return it != behavior->instances.end() ? sol::object(it->second.vars) : sol::object(sol::lua_nil);
        }),
        "get", [this](const LuaBehaviorTree& bt, const std::string& name) -> sol::object {
            if (behavior) return behavior->get(bt.handle, name);
            return sol::lua_nil;
        },
        "set", [this](const LuaBehaviorTree& bt, const std::string& name, const sol::object& value) {
            if (behavior) behavior->set(bt.handle, name, value);
        },
        "has", [comp](const LuaBehaviorTree& bt, const std::string& name) {
            const ai::BehaviorTree* c = comp(bt);
            return c != nullptr && c->runtime.find(name) != nullptr;
        },
        "clear", [comp](const LuaBehaviorTree& bt, const std::string& name) {
            ai::BehaviorTree* c = comp(bt);
            if (c == nullptr) return;
            if (ai::Value* v = c->runtime.find(name)) {
                v->b = false;
                v->n = 0.0;
                v->s.clear();
                v->v = Vec3{};
                v->entity = ai::kNoEntity;
            }
        },
        "isActive", [this, comp](const LuaBehaviorTree& bt, const std::string& node) {
            const ai::BehaviorTree* c = comp(bt);
            BtHost::Tree* t = behavior ? behavior->treeOf(bt.handle) : nullptr;
            if (c == nullptr || t == nullptr) return false;
            for (const int n : c->runtime.active_path) {
                if (n >= 0 && n < static_cast<int>(t->data.nodes.size()) && t->data.nodes[n].title() == node) return true;
            }
            return false;
        },
        "finishTask", [comp](const LuaBehaviorTree& bt, sol::optional<bool> success) {
            if (ai::BehaviorTree* c = comp(bt)) c->runtime.pending_finish = success.value_or(true) ? ai::BtStatus::Success : ai::BtStatus::Failure;
        },
        "start", [comp](const LuaBehaviorTree& bt) {
            if (ai::BehaviorTree* c = comp(bt)) c->runtime.running = true;
        },
        "stop", [this, comp](const LuaBehaviorTree& bt) {
            ai::BehaviorTree* c = comp(bt);
            if (c == nullptr) return;
            if (behavior) behavior->abort(bt.handle);
            c->runtime.running = false;
        },
        "restart", [this](const LuaBehaviorTree& bt) {
            if (behavior) behavior->restart(bt.handle);
        },
        sol::meta_function::to_string, [comp, nodeName](const LuaBehaviorTree& bt) {
            const ai::BehaviorTree* c = comp(bt);
            const sol::object s = c != nullptr ? nodeName(bt, c->runtime.active_task) : sol::object(sol::lua_nil);
            return std::string("BehaviorTree(") + (s.is<std::string>() ? s.as<std::string>() : std::string("-")) + ")";
        });
    // BehaviorTree.of(entity), registerTask, reportNoise.
    type["of"] = [this](const LuaEntity& e) -> sol::object {
        if (!e.valid() || !e.get().has<ai::BehaviorTree>()) return sol::lua_nil;
        return sol::make_object(*lua, LuaBehaviorTree{e.handle});
    };
    type["registerTask"] = [this](const std::string& name, const sol::object& task) {
        if (!behavior || name.empty()) return false;
        if (task.get_type() != sol::type::function && task.get_type() != sol::type::table) {
            write(1, "BehaviorTree.registerTask(\"" + name + "\"): hace falta una funcion o una tabla {start, update, abort}");
            return false;
        }
        behavior->tasks[name] = task;
        behavior->warned.erase(name);
        return true;
    };
    type["reportNoise"] = [this](const Vec3& position, sol::optional<float> radius, sol::optional<LuaEntity> instigator,
                                 sol::optional<std::string> tag) {
        if (!behavior) return;
        ai::BtNoise noise;
        noise.position = position;
        noise.radius = radius.value_or(10.0f);
        noise.time = time;
        if (instigator && instigator->valid()) noise.instigator = static_cast<std::uint32_t>(entt::to_integral(instigator->handle));
        noise.tag = tag.value_or(std::string{});
        behavior->noises.push_back(std::move(noise));
        if (behavior->noises.size() > 256) behavior->noises.erase(behavior->noises.begin());
    };
    type["broadcast"] = [this](const std::string& key, const sol::object& value) {
        if (world == nullptr || !behavior) return 0;
        int count = 0;
        for (const entt::entity h : world->registry().view<ai::BehaviorTree>()) {
            if (world->registry().get<ai::BehaviorTree>(h).runtime.find(key) == nullptr) continue;
            behavior->set(h, key, value);
            ++count;
        }
        return count;
    };
    entity["getBehaviorTree"] = [this](const LuaEntity& e) -> sol::object {
        if (!e.valid() || !e.get().has<ai::BehaviorTree>()) return sol::lua_nil;
        return sol::make_object(*lua, LuaBehaviorTree{e.handle});
    };
}
