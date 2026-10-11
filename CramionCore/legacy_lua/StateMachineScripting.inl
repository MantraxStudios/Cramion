// Maquinas de estados de IA en Lua (ai/StateMachine.h). Se incluye en
// Scripting.cpp despues de ScriptSystem::Impl: usa su estado de Lua, sus
// entidades (LuaEntity) y sus mensajes de error.
//
// Cada objeto con el componente StateMachine tiene un `self` (una tabla, la
// misma en todos sus estados) con self.entity, self.sm, self.vars (la
// pizarra) y self.state. Al entrar en un estado, self busca primero en el
// codigo de ese estado y despues en el de "Cualquier estado" (sus funciones
// sirven en todos) y en las globales. El codigo de un estado define
// OnEnter(self, sm), OnUpdate(self, dt), OnExit(self)... como funciones
// globales del estado o devuelve una tabla con ellas (como un script).

namespace {
// sm en Lua: la maquina de un objeto.
struct LuaStateMachine {
    entt::entity handle = entt::null;
};
}  // namespace

struct ScriptSystem::Impl::FsmHost {
    ScriptSystem::Impl& d;
    explicit FsmHost(ScriptSystem::Impl& impl) : d(impl) {}

    // Un .crfsm cargado y compilado.
    struct Machine {
        std::filesystem::path path;
        std::string relative;  // "IA/Enemigo.crfsm" (para los errores y la recarga)
        ai::StateMachineAsset data;
        sol::object any_class;               // codigo de Cualquier estado
        std::vector<sol::object> classes;    // por estado (nil si no compila)
        std::map<std::pair<int, int>, sol::protected_function> conditions;  // expresiones Lua
        std::set<std::pair<int, int>> bad_conditions;  // con error (se avisa una vez)
    };
    std::unordered_map<Uuid, Machine> machines;
    std::unordered_map<Uuid, std::filesystem::path> files;  // .crfsm de Assets por UUID
    std::uint64_t indexed_frame = ~0ull;
    std::set<Uuid> missing;  // sin archivo: no se busca cada frame (hasta recargar)

    struct Instance {
        Uuid machine{};
        sol::table self;
        sol::table vars;             // pizarra (proxy)
        sol::environment cond_env;   // entorno de las expresiones de las condiciones
        std::set<int> failed;        // estados con error hasta que se recargue (-1 = Cualquier estado)
    };
    std::unordered_map<entt::entity, Instance> instances;

    // --- Assets ---
    std::string relativeOf(const std::filesystem::path& file) const {
        std::error_code e;
        const std::filesystem::path rel = std::filesystem::relative(file, d.root, e);
        const std::u8string text = (e ? file.filename() : rel).generic_u8string();
        return std::string(text.begin(), text.end());
    }

    void indexFiles() {
        if (indexed_frame == d.frame) return;  // como mucho una vez por frame
        indexed_frame = d.frame;
        files.clear();
        std::error_code e;
        if (d.root.empty() || !std::filesystem::is_directory(d.root, e)) return;
        for (std::filesystem::recursive_directory_iterator it(d.root, std::filesystem::directory_options::skip_permission_denied, e);
             !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e)) {
            if (it->path().extension() != ai::kStateMachineExtension) continue;
            std::ifstream in(it->path(), std::ios::binary);
            const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_object() && j.contains("uuid") && j["uuid"].is_string()) {
                const Uuid uuid = Uuid::parse(j["uuid"].get<std::string>());
                if (uuid.valid()) files[uuid] = it->path();
            }
        }
    }

    Machine* machine(const Uuid& uuid) {
        if (const auto it = machines.find(uuid); it != machines.end()) return &it->second;
        if (missing.contains(uuid)) return nullptr;
        auto file = files.find(uuid);
        if (file == files.end()) {
            indexFiles();
            file = files.find(uuid);
            if (file == files.end()) {
                missing.insert(uuid);
                d.write(1, "Maquina de estados: no se encuentra el .crfsm " + uuid.toString());
                return nullptr;
            }
        }
        Machine m;
        m.path = file->second;
        m.relative = relativeOf(m.path);
        std::string error;
        if (!ai::loadStateMachine(m.path, m.data, &error)) {
            d.write(2, "Maquina de estados: " + error);
            return nullptr;
        }
        for (const std::string& problem : ai::validateStateMachine(m.data)) {
            d.write(1, "Maquina de estados " + m.relative + ": " + problem);
        }
        compile(m);
        return &(machines[uuid] = std::move(m));
    }

    // "IA/Enemigo.crfsm#Perseguir": nombre del trozo de Lua de un estado (los
    // errores salen con el y con la linea; el editor los marca en el codigo).
    static std::string chunkName(const Machine& m, int state) {
        return m.relative + "#" + (state < 0 ? std::string("Cualquier estado") : m.data.states[state].name);
    }

    void report(const Machine& m, int state, const std::string& message) {
        const std::string chunk = chunkName(m, state);
        ScriptError error;
        error.file = chunk;
        // La primera aparicion es la linea del error (las siguientes son del
        // traceback: "in function <...#Estado:1>").
        const std::string key = chunk + ":";
        if (const std::size_t at = message.find(key); at != std::string::npos) {
            error.line = std::atoi(message.c_str() + at + key.size());
        }
        error.message = message;
        if (d.errors.size() > 64) d.errors.erase(d.errors.begin());
        d.errors.push_back(error);
        const std::string where = state < 0 ? std::string("Cualquier estado") : "estado " + m.data.states[state].name;
        d.write(2, "Maquina " + m.relative + " (" + where + (error.line > 0 ? ", linea " + std::to_string(error.line) : "") +
                       "): " + message);
    }

    // Codigo de un estado -> su tabla. `fallback`: donde busca lo que no
    // define (las globales o el codigo de Cualquier estado).
    sol::object compileCode(Machine& m, int state, const std::string& code, const std::string& script,
                            const sol::table& fallback) {
        sol::state& L = *d.lua;
        if (!script.empty()) {
            std::string error;
            sol::object cls = d.loadClass(L, script, &error);
            if (!cls.is<sol::table>()) {
                d.fail(script, error);
                return sol::lua_nil;
            }
            return cls;
        }
        sol::environment env(L.lua_state(), sol::create, fallback);
        sol::load_result chunk = L.load(code, "@" + chunkName(m, state));
        if (!chunk.valid()) {
            sol::error e = chunk;
            report(m, state, e.what());
            return sol::lua_nil;
        }
        sol::protected_function f = chunk;
        env.set_on(f);
        sol::protected_function_result r = f();
        if (!r.valid()) {
            sol::error e = r;
            report(m, state, e.what());
            return sol::lua_nil;
        }
        sol::object returned = r;
        if (returned.is<sol::table>()) return returned;
        return sol::make_object(L, sol::table(env));
    }

    void compile(Machine& m) {
        m.conditions.clear();
        m.bad_conditions.clear();
        m.classes.clear();
        m.any_class = compileCode(m, -1, m.data.any_code, {}, d.lua->globals());
        const sol::table fallback = m.any_class.is<sol::table>() ? m.any_class.as<sol::table>() : sol::table(d.lua->globals());
        for (int i = 0; i < static_cast<int>(m.data.states.size()); ++i) {
            m.classes.push_back(compileCode(m, i, m.data.states[i].code, m.data.states[i].script, fallback));
        }
    }

    // --- Pizarra <-> Lua ---
    entt::entity resolveEntity(const std::string& text) const {
        if (d.world == nullptr || text.empty()) return entt::null;
        if (const Uuid uuid = Uuid::parse(text); uuid.valid()) {
            if (const ecs::Entity e = d.world->find(uuid); e.valid()) return e.handle();
        }
        if (const ecs::Entity e = d.world->findByName(text); e.valid()) return e.handle();
        if (const ecs::Entity e = d.world->findWithTag(text); e.valid()) return e.handle();
        return entt::null;
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

    // Lua -> la variable (con su tipo). false si el valor no vale para ese tipo.
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

    // --- Componente ---
    ai::StateMachine* component(entt::entity h) const {
        if (d.world == nullptr || !d.world->registry().valid(h)) return nullptr;
        return d.world->registry().try_get<ai::StateMachine>(h);
    }

    Machine* machineOf(entt::entity h) {
        const auto it = instances.find(h);
        if (it == instances.end()) {
            const ai::StateMachine* c = component(h);
            return c != nullptr && c->machine.valid() ? machine(c->machine.uuid) : nullptr;
        }
        return machine(it->second.machine);
    }

    sol::object get(entt::entity h, const std::string& name) {
        const ai::StateMachine* c = component(h);
        if (c == nullptr) return sol::lua_nil;
        const ai::Value* v = c->runtime.find(name);
        return v != nullptr ? toLua(*v) : sol::object(sol::lua_nil);
    }

    void set(entt::entity h, const std::string& name, const sol::object& value) {
        ai::StateMachine* c = component(h);
        if (c == nullptr) return;
        if (ai::Value* v = c->runtime.find(name)) {
            if (!fromLua(value, *v)) {
                d.write(1, "Maquina de estados: la variable \"" + name + "\" es " + ai::varTypeLabel(v->type) +
                               " (no se puede poner un " + std::string(sol::type_name(d.lua->lua_state(), value.get_type())) + ")");
            }
            return;
        }
        // Variable nueva: el tipo sale del valor.
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
            d.write(1, "Maquina de estados: \"" + name + "\" no puede guardar ese valor (bool, numero, texto, entidad o Vec3)");
            return;
        }
        if (fromLua(value, var.value)) c->runtime.vars.push_back(std::move(var));
    }

    // --- Instancias ---
    Instance& instance(entt::entity h, ai::StateMachine& c, Machine& m) {
        if (auto it = instances.find(h); it != instances.end() && it->second.machine == c.machine.uuid) return it->second;
        sol::state& L = *d.lua;
        Instance inst;
        inst.machine = c.machine.uuid;
        inst.self = L.create_table();
        inst.self["entity"] = LuaEntity{h, d.world};
        inst.self["sm"] = LuaStateMachine{h};
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
        // Expresiones: las variables por su nombre, entity, sm, self, vars y las globales.
        inst.cond_env = sol::environment(L.lua_state(), sol::create);
        sol::table env_meta = L.create_table();
        const sol::table self = inst.self;
        env_meta["__index"] = [this, h, self](const sol::table&, const sol::object& key, sol::this_state s) -> sol::object {
            if (key.get_type() != sol::type::string) return sol::lua_nil;
            const std::string k = key.as<std::string>();
            if (const ai::StateMachine* sc = component(h); sc != nullptr && sc->runtime.find(k) != nullptr) return get(h, k);
            sol::table t = self;
            if (k == "self") return t;
            if (k == "entity" || k == "sm" || k == "vars") return t.raw_get<sol::object>(k);
            return sol::state_view(s).globals().get<sol::object>(k);
        };
        inst.cond_env[sol::metatable_key] = env_meta;
        // Pizarra inicial: la del asset con lo del Inspector; las entity, por su nombre.
        ai::resetRuntime(m.data, c.runtime, c.variables);
        c.runtime.running = c.start_active;
        resolveEntities(c.runtime);
        instances[h] = std::move(inst);
        return instances[h];
    }

    void resolveEntities(ai::Runtime& rt) const {
        for (ai::Variable& v : rt.vars) {
            if (v.value.type != ai::VarType::Entity) continue;
            const entt::entity h = resolveEntity(v.value.s);
            v.value.entity = h == entt::null ? ai::kNoEntity : static_cast<std::uint32_t>(entt::to_integral(h));
        }
    }

    // Llama a un metodo del codigo de un estado (-1 = Cualquier estado). Solo
    // lo que define ese codigo (no lo heredado de Cualquier estado).
    template <typename... Args>
    void call(Instance& inst, Machine& m, int state, const char* method, Args&&... args) {
        if (inst.failed.contains(state)) return;
        if (state >= static_cast<int>(m.classes.size())) return;
        const sol::object& cls = state < 0 ? m.any_class : m.classes[state];
        if (!cls.is<sol::table>()) return;
        sol::object fn = cls.as<sol::table>().raw_get<sol::object>(method);
        if (fn.get_type() != sol::type::function) return;
        sol::protected_function pf = fn;
        sol::protected_function_result r = pf(inst.self, std::forward<Args>(args)...);
        if (!r.valid()) {
            sol::error e = r;
            inst.failed.insert(state);
            const std::string chunk = chunkName(m, state);
            // Un estado que viene de un .lua: el error ya trae su archivo.
            if (state >= 0 && !m.data.states[state].script.empty()) {
                d.fail(m.data.states[state].script, e.what());
            } else {
                report(m, state, e.what());
            }
        }
    }

    // self pasa a buscar en el codigo del estado actual.
    void setCurrent(Instance& inst, Machine& m, int state) {
        sol::table meta = d.lua->create_table();
        if (state >= 0 && state < static_cast<int>(m.classes.size()) && m.classes[state].is<sol::table>()) {
            meta["__index"] = m.classes[state];
        } else if (m.any_class.is<sol::table>()) {
            meta["__index"] = m.any_class;
        } else {
            meta["__index"] = d.lua->globals();
        }
        inst.self[sol::metatable_key] = meta;
        inst.self["state"] = state >= 0 && state < static_cast<int>(m.data.states.size()) ? sol::make_object(*d.lua, m.data.states[state].name)
                                                                                         : sol::object(sol::lua_nil);
    }

    void applyChange(entt::entity h, ai::StateMachine& c, Machine& m, Instance& inst, const ai::StepResult& r) {
        if (!r.changed) return;
        if (r.from >= 0) call(inst, m, r.from, "OnExit");
        setCurrent(inst, m, r.to);
        if (c.debug) {
            const std::string from = r.from >= 0 ? m.data.states[r.from].name : std::string("Entrada");
            std::string why = r.transition == -2 ? std::string("sm:go") : (r.transition >= 0 ? "transicion " + std::to_string(r.transition + 1) : std::string("entrada"));
            const ecs::Entity e = d.world->wrap(h);
            d.write(0, "[Maquina] " + e.name() + ": " + from + " -> " + m.data.states[r.to].name + " (" + why + ")");
        }
        call(inst, m, r.to, "OnEnter", LuaStateMachine{h});
    }

    // sm:go pedidos en OnEnter/OnUpdate: se aplican ya (como mucho 8 seguidos).
    void settle(entt::entity h, ai::StateMachine& c, Machine& m, Instance& inst) {
        for (int i = 0; i < 8 && c.runtime.requested >= 0; ++i) {
            applyChange(h, c, m, inst, ai::applyRequestedState(m.data, c.runtime));
        }
        c.runtime.requested = -1;
    }

    bool evalCondition(Machine& m, Instance& inst, int transition, int index, const std::string& expression) {
        const std::pair<int, int> key{transition, index};
        if (m.bad_conditions.contains(key)) return false;
        auto it = m.conditions.find(key);
        if (it == m.conditions.end()) {
            const std::string name = m.relative + "#transicion " + std::to_string(transition + 1);
            sol::load_result chunk = d.lua->load("return (" + expression + ")", "@" + name);
            if (!chunk.valid()) {
                sol::error e = chunk;
                m.bad_conditions.insert(key);
                d.write(2, "Maquina " + m.relative + " (transicion " + std::to_string(transition + 1) + "): " + e.what());
                return false;
            }
            it = m.conditions.emplace(key, sol::protected_function(chunk)).first;
        }
        inst.cond_env.set_on(it->second);
        sol::protected_function_result r = it->second();
        if (!r.valid()) {
            sol::error e = r;
            m.bad_conditions.insert(key);
            d.write(2, "Maquina " + m.relative + " (transicion " + std::to_string(transition + 1) + "): " + e.what());
            return false;
        }
        const sol::object value = r;
        return value.valid() && value.get_type() != sol::type::lua_nil &&
               !(value.get_type() == sol::type::boolean && !value.as<bool>());
    }

    // Un frame de una maquina: sensores (Cualquier estado), transiciones,
    // OnUpdate del estado y los sm:go que pidio.
    void tick(entt::entity h, ai::StateMachine& c, Machine& m, Instance& inst, float dt) {
        ai::Runtime& rt = c.runtime;
        if (!rt.running || m.data.states.empty()) return;
        if (!rt.started) {
            call(inst, m, -1, "OnEnter", LuaStateMachine{h});
            const int requested = rt.requested;  // un sm:go antes de empezar: despues de entrar
            applyChange(h, c, m, inst, ai::startStateMachine(m.data, rt));
            if (requested >= 0 && rt.requested < 0) rt.requested = requested;
            settle(h, c, m, inst);
        }
        call(inst, m, -1, "OnUpdate", dt);
        const ai::LuaConditionFn lua = [&](int t, int k, const std::string& expression) {
            return evalCondition(m, inst, t, k, expression);
        };
        applyChange(h, c, m, inst, ai::stepStateMachine(m.data, rt, dt, lua));
        settle(h, c, m, inst);
        if (rt.state >= 0) call(inst, m, rt.state, "OnUpdate", dt);
        settle(h, c, m, inst);
    }

    // Las maquinas en orden fijo (por entidad): el mismo resultado cada vez.
    std::vector<entt::entity> ordered(ecs::World& world) const {
        std::vector<entt::entity> out;
        for (const entt::entity h : world.registry().view<ai::StateMachine>()) out.push_back(h);
        std::sort(out.begin(), out.end(), [](entt::entity a, entt::entity b) { return entt::to_integral(a) < entt::to_integral(b); });
        return out;
    }

    void update(ecs::World& world, float dt) {
        for (auto it = instances.begin(); it != instances.end();) {
            const ai::StateMachine* c = component(it->first);
            if (c == nullptr || c->machine.uuid != it->second.machine) it = instances.erase(it);
            else ++it;
        }
        for (const entt::entity h : ordered(world)) {
            const ecs::Entity e = world.wrap(h);
            if (!e.activeInHierarchy()) continue;
            ai::StateMachine* c = component(h);
            if (c == nullptr || !c->machine.valid()) continue;
            Machine* m = machine(c->machine.uuid);
            if (m == nullptr) continue;
            Instance& inst = instance(h, *c, *m);
            tick(h, *c, *m, inst, dt);
        }
    }

    // LateUpdate / FixedUpdate / fisica: Cualquier estado y el estado actual.
    template <typename... Args>
    void forward(entt::entity h, const char* method, Args&&... args) {
        const auto it = instances.find(h);
        if (it == instances.end()) return;
        ai::StateMachine* c = component(h);
        if (c == nullptr || !c->runtime.started || !c->runtime.running) return;
        Machine* m = machine(it->second.machine);
        if (m == nullptr) return;
        call(it->second, *m, -1, method, args...);
        if (c->runtime.state >= 0) call(it->second, *m, c->runtime.state, method, args...);
        settle(h, *c, *m, it->second);
    }

    template <typename... Args>
    void forwardAll(const char* method, Args&&... args) {
        if (d.world == nullptr) return;
        for (const entt::entity h : ordered(*d.world)) {
            if (instances.contains(h) && d.world->wrap(h).activeInHierarchy()) forward(h, method, args...);
        }
    }

    // Guardar un .crfsm o un .lua en Play: se recompila; cada maquina sigue en
    // su estado con sus variables (las nuevas del asset se anaden).
    void reload(const std::string& file) {
        const bool fsm = std::filesystem::path(file).extension() == ai::kStateMachineExtension;
        indexed_frame = ~0ull;
        missing.clear();
        for (auto& [uuid, m] : machines) {
            bool uses = false;
            if (fsm) {
                uses = m.relative == file;
            } else {
                for (const ai::State& s : m.data.states) uses = uses || s.script == file;
            }
            if (!uses) continue;
            if (fsm) {
                ai::StateMachineAsset fresh;
                std::string error;
                if (!ai::loadStateMachine(m.path, fresh, &error)) {
                    d.write(2, "Maquina de estados: " + error);
                    continue;
                }
                m.data = std::move(fresh);
            }
            compile(m);
            for (auto& [h, inst] : instances) {
                if (inst.machine != uuid) continue;
                inst.failed.clear();
                ai::StateMachine* c = component(h);
                if (c == nullptr) continue;
                ai::Runtime& rt = c->runtime;
                if (rt.state >= static_cast<int>(m.data.states.size())) {
                    rt.started = false;  // ese estado ya no existe: empieza otra vez
                    rt.state = -1;
                }
                for (const ai::Variable& v : m.data.variables) {
                    if (rt.find(v.name) == nullptr) rt.vars.push_back(v);
                }
                resolveEntities(rt);
                if (rt.state >= 0) setCurrent(inst, m, rt.state);
            }
            d.write(0, "Maquina de estados recargada: " + m.relative);
        }
    }
};

void ScriptSystem::Impl::fsmEvent(entt::entity target, const char* method, entt::entity other, const sol::table& contact) {
    if (fsm) fsm->forward(target, method, LuaEntity{other, world}, contact);
}

void ScriptSystem::Impl::bindStateMachine(sol::state& L, sol::usertype<LuaEntity>& entity) {
    // Con o sin Play: sin maquinas en marcha, la pizarra del componente igual se lee y se cambia.
    const auto comp = [this](const LuaStateMachine& sm) -> ai::StateMachine* {
        if (world == nullptr || !world->registry().valid(sm.handle)) return nullptr;
        return world->registry().try_get<ai::StateMachine>(sm.handle);
    };
    const auto stateIndex = [this](const LuaStateMachine& sm, const std::string& name) -> int {
        FsmHost::Machine* m = fsm ? fsm->machineOf(sm.handle) : nullptr;
        return m != nullptr ? m->data.findState(name) : -1;
    };
    const auto stateName = [this](const LuaStateMachine& sm, int index) -> sol::object {
        FsmHost::Machine* m = fsm ? fsm->machineOf(sm.handle) : nullptr;
        if (m == nullptr || index < 0 || index >= static_cast<int>(m->data.states.size())) return sol::lua_nil;
        return sol::make_object(*lua, m->data.states[index].name);
    };
    const auto go = [this, comp, stateIndex](const LuaStateMachine& sm, const std::string& name) {
        ai::StateMachine* c = comp(sm);
        if (c == nullptr) return false;
        const int index = stateIndex(sm, name);
        if (index < 0) {
            write(1, "sm:go: no hay un estado \"" + name + "\"");
            return false;
        }
        c->runtime.requested = index;
        return true;
    };
    auto type = L.new_usertype<LuaStateMachine>(
        "StateMachine", sol::no_constructor,
        "state", sol::property([comp, stateName](const LuaStateMachine& sm) -> sol::object {
            const ai::StateMachine* c = comp(sm);
            return c != nullptr ? stateName(sm, c->runtime.state) : sol::object(sol::lua_nil);
        }),
        "previous", sol::property([comp, stateName](const LuaStateMachine& sm) -> sol::object {
            const ai::StateMachine* c = comp(sm);
            return c != nullptr ? stateName(sm, c->runtime.previous) : sol::object(sol::lua_nil);
        }),
        "stateTime", sol::property([comp](const LuaStateMachine& sm) {
            const ai::StateMachine* c = comp(sm);
            return c != nullptr ? c->runtime.state_time : 0.0f;
        }),
        "time", sol::property([comp](const LuaStateMachine& sm) {
            const ai::StateMachine* c = comp(sm);
            return c != nullptr ? c->runtime.time : 0.0f;
        }),
        "changes", sol::property([comp](const LuaStateMachine& sm) {
            const ai::StateMachine* c = comp(sm);
            return c != nullptr ? static_cast<double>(c->runtime.changes) : 0.0;
        }),
        "running", sol::property([comp](const LuaStateMachine& sm) {
            const ai::StateMachine* c = comp(sm);
            return c != nullptr && c->runtime.running;
        }),
        "entity", sol::property([this](const LuaStateMachine& sm) { return LuaEntity{sm.handle, world}; }),
        "vars", sol::property([this](const LuaStateMachine& sm) -> sol::object {
            if (!fsm) return sol::lua_nil;
            const auto it = fsm->instances.find(sm.handle);
            return it != fsm->instances.end() ? sol::object(it->second.vars) : sol::object(sol::lua_nil);
        }),
        "go", go,
        "changeState", go,
        "isIn", [comp, stateIndex](const LuaStateMachine& sm, const std::string& name) {
            const ai::StateMachine* c = comp(sm);
            return c != nullptr && c->runtime.state >= 0 && c->runtime.state == stateIndex(sm, name);
        },
        "trigger", [comp](const LuaStateMachine& sm, const std::string& name) {
            if (ai::StateMachine* c = comp(sm)) c->runtime.triggers.push_back(name);
        },
        "get", [this](const LuaStateMachine& sm, const std::string& name) -> sol::object {
            if (fsm) return fsm->get(sm.handle, name);
            if (world == nullptr || !world->registry().valid(sm.handle)) return sol::lua_nil;
            const ai::StateMachine* c = world->registry().try_get<ai::StateMachine>(sm.handle);
            const ai::Value* v = c != nullptr ? c->runtime.find(name) : nullptr;
            return v != nullptr ? sol::make_object(*lua, v->text()) : sol::object(sol::lua_nil);
        },
        "set", [this](const LuaStateMachine& sm, const std::string& name, const sol::object& value) {
            if (fsm) fsm->set(sm.handle, name, value);
        },
        "has", [comp](const LuaStateMachine& sm, const std::string& name) {
            const ai::StateMachine* c = comp(sm);
            return c != nullptr && c->runtime.find(name) != nullptr;
        },
        "states", [this](const LuaStateMachine& sm) {
            sol::table out = lua->create_table();
            if (FsmHost::Machine* m = fsm ? fsm->machineOf(sm.handle) : nullptr) {
                for (std::size_t i = 0; i < m->data.states.size(); ++i) out[i + 1] = m->data.states[i].name;
            }
            return out;
        },
        "start", [comp](const LuaStateMachine& sm) {
            if (ai::StateMachine* c = comp(sm)) c->runtime.running = true;
        },
        "stop", [comp](const LuaStateMachine& sm) {
            if (ai::StateMachine* c = comp(sm)) c->runtime.running = false;
        },
        "restart", [this, comp](const LuaStateMachine& sm) {
            ai::StateMachine* c = comp(sm);
            if (c == nullptr) return;
            if (fsm) {
                const auto it = fsm->instances.find(sm.handle);
                FsmHost::Machine* m = fsm->machineOf(sm.handle);
                if (it != fsm->instances.end() && m != nullptr && c->runtime.started && c->runtime.state >= 0) {
                    fsm->call(it->second, *m, c->runtime.state, "OnExit");
                }
                if (m != nullptr) {
                    ai::resetRuntime(m->data, c->runtime, c->variables);
                    fsm->resolveEntities(c->runtime);
                }
            }
            c->runtime.running = true;
            c->runtime.started = false;
        },
        sol::meta_function::to_string, [comp, stateName](const LuaStateMachine& sm) {
            const ai::StateMachine* c = comp(sm);
            const sol::object s = c != nullptr ? stateName(sm, c->runtime.state) : sol::object(sol::lua_nil);
            return std::string("StateMachine(") + (s.is<std::string>() ? s.as<std::string>() : std::string("-")) + ")";
        });
    // StateMachine.of(entity) y StateMachine.broadcast("trigger"): todas las maquinas.
    type["of"] = [this](const LuaEntity& e) -> sol::object {
        if (!e.valid() || !e.get().has<ai::StateMachine>()) return sol::lua_nil;
        return sol::make_object(*lua, LuaStateMachine{e.handle});
    };
    type["broadcast"] = [this](const std::string& name) {
        if (world == nullptr) return 0;
        int count = 0;
        for (const entt::entity h : world->registry().view<ai::StateMachine>()) {
            world->registry().get<ai::StateMachine>(h).runtime.triggers.push_back(name);
            ++count;
        }
        return count;
    };
    entity["getStateMachine"] = [this](const LuaEntity& e) -> sol::object {
        if (!e.valid() || !e.get().has<ai::StateMachine>()) return sol::lua_nil;
        return sol::make_object(*lua, LuaStateMachine{e.handle});
    };
}
