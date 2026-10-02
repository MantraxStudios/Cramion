// Visual Scripting (scripting/VisualScript.h). Se incluye en Scripting.cpp
// despues de ScriptSystem::Impl: cada .crgraph se compila a una clase Lua
// (vscript::compileGraph) y corre en el mismo estado que los scripts, asi que
// llega a toda la API. Las instancias son aparte de las de los scripts: un
// objeto puede tener Script, VisualScript y StateMachine a la vez.
//
// Depuracion (solo si el programa la activa: el editor en Play): cada nodo
// que se ejecuta guarda su hora en self.__t y los pines su ultimo valor en
// self.__o; los puntos de ruptura (__VS_BP) llaman a __VS_BREAK y el editor
// pausa el Play al terminar el frame.

struct ScriptSystem::Impl::VsHost {
    ScriptSystem::Impl& d;
    explicit VsHost(ScriptSystem::Impl& impl) : d(impl) {}

    std::unordered_map<std::string, sol::table> classes;   // .crgraph -> clase compilada
    std::unordered_map<std::string, std::string> failed;   // .crgraph -> error (hasta recargar)
    std::unordered_map<entt::entity, Instance> instances;
    std::map<std::string, std::set<int>> file_breakpoints;  // los guardados en cada .crgraph

    // .crgraph -> clase Lua (nil + error si no se puede leer o tiene errores).
    sol::object load(const std::string& file, std::string* error) {
        vscript::Graph graph;
        std::string problem;
        if (!vscript::loadGraph(d.root / fromUtf8(file), graph, &problem)) {
            if (error) *error = file + ": " + problem;
            return sol::lua_nil;
        }
        std::set<int>& points = file_breakpoints[file];
        points.clear();
        for (const vscript::Node& n : graph.nodes) {
            if (n.breakpoint) points.insert(n.id);
        }
        installBreakpoints();
        const vscript::CompileResult compiled = vscript::compileGraph(graph, file);
        if (!compiled.ok) {
            std::string text = file + ": el grafo tiene errores (abrelo en la ventana Visual Script)";
            for (std::size_t i = 0; i < compiled.errors.size() && i < 4; ++i) {
                const vscript::NodeError& e = compiled.errors[i];
                text += "\n  " + (e.node > 0 ? "nodo " + std::to_string(e.node) + ": " : std::string()) + e.message;
            }
            if (error) *error = text;
            return sol::lua_nil;
        }
        sol::state& L = *d.lua;
        sol::load_result chunk = L.load(compiled.lua, "@" + file);
        if (!chunk.valid()) {
            sol::error e = chunk;
            if (error) *error = e.what();
            return sol::lua_nil;
        }
        sol::protected_function f = chunk;
        sol::protected_function_result r = f();
        if (!r.valid()) {
            sol::error e = r;
            if (error) *error = e.what();
            return sol::lua_nil;
        }
        sol::object value = r;
        if (!value.is<sol::table>()) {
            if (error) *error = file + ": el grafo no genero una clase";
            return sol::lua_nil;
        }
        return value;
    }

    sol::table* classFor(const std::string& file) {
        if (const auto it = classes.find(file); it != classes.end()) return &it->second;
        if (failed.contains(file)) return nullptr;
        std::string error;
        sol::object cls = load(file, &error);
        if (!cls.is<sol::table>()) {
            failed[file] = error;
            d.fail(file, error);
            return nullptr;
        }
        return &(classes[file] = cls.as<sol::table>());
    }

    // Tabla global __VS_BP: .crgraph -> {nodo = true}. Sin depuracion, nil
    // (el juego exportado no se para nunca).
    void installBreakpoints() {
        if (!d.lua) return;
        sol::state& L = *d.lua;
        if (!d.vs_debugging) {
            L["__VS_BP"] = sol::lua_nil;
            return;
        }
        std::map<std::string, std::set<int>> all = file_breakpoints;
        for (const auto& [file, nodes] : d.vs_breakpoints) all[file] = nodes;  // las del editor mandan
        sol::table table = L.create_table();
        for (const auto& [file, nodes] : all) {
            if (nodes.empty()) continue;
            sol::table points = L.create_table();
            for (const int id : nodes) points[id] = true;
            table[file] = points;
        }
        L["__VS_BP"] = table;
    }

    void bind() {
        sol::state& L = *d.lua;
        ScriptSystem::Impl& impl = d;
        L.set_function("__VS_BREAK", [&impl](const std::string& file, int node, sol::object entity) {
            if (!impl.vs_debugging || impl.vs_break.pending) return;
            impl.vs_break.graph = file;
            impl.vs_break.node = node;
            impl.vs_break.entity = entity.is<LuaEntity>() ? entity.as<LuaEntity>().handle : entt::null;
            impl.vs_break.pending = true;
            impl.write(1, "Visual Script: punto de ruptura en " + file + " (nodo " + std::to_string(node) + ")");
        });
        // Send Event: el metodo del Visual Script y del script de otro objeto.
        L.set_function("__VS_SEND", [&impl](sol::object target, const std::string& method, sol::object value) {
            if (!target.is<LuaEntity>() || method.empty()) return;
            const entt::entity handle = target.as<LuaEntity>().handle;
            if (Instance* visual = impl.vsInstance(handle)) impl.call(*visual, method.c_str(), value);
            if (const auto it = impl.instances.find(handle); it != impl.instances.end()) {
                impl.call(it->second, method.c_str(), value);
            }
        });
    }

    void createInstance(ecs::Entity e, const vscript::VisualScript& component) {
        if (component.graph.empty()) return;
        sol::table* cls = classFor(component.graph);
        if (cls == nullptr) return;
        Instance inst;
        inst.file = component.graph;
        inst.self = d.lua->create_table();
        sol::table meta = d.lua->create_table();
        meta["__index"] = *cls;
        inst.self[sol::metatable_key] = meta;
        inst.self["entity"] = LuaEntity{e.handle(), d.world};
        // Variables de este objeto (Inspector); el resto, las del grafo (__vsinit).
        for (const ScriptProperty& p : component.properties) {
            if (!p.name.empty()) inst.self[p.name] = d.propertyValue(p);
        }
        Instance& stored = instances[e.handle()] = std::move(inst);
        d.call(stored, "Awake");
    }

    bool runnable(entt::entity handle) const {
        if (d.world == nullptr || !d.world->registry().valid(handle)) return false;
        const vscript::VisualScript* c = d.world->registry().try_get<vscript::VisualScript>(handle);
        return c != nullptr && c->enabled && d.world->wrap(handle).activeInHierarchy();
    }

    std::vector<entt::entity> order() const {
        std::vector<entt::entity> list;
        list.reserve(instances.size());
        for (const auto& [handle, inst] : instances) list.push_back(handle);
        return list;
    }
};

void ScriptSystem::Impl::vsStart() {
    vs = std::make_unique<VsHost>(*this);
    vs_break = VsBreak{};
    vs->bind();
    vs->installBreakpoints();
    if (world == nullptr) return;
    for (const entt::entity handle : world->registry().view<vscript::VisualScript>()) {
        const ecs::Entity e = world->wrap(handle);
        if (e.activeInHierarchy()) vs->createInstance(e, e.get<vscript::VisualScript>());
    }
}

void ScriptSystem::Impl::vsUpdate(int phase, float dt) {
    if (!vs || world == nullptr) return;
    VsHost& h = *vs;
    if (phase == 0) {
        // Objetos con Visual Script nuevos (creados en Play) y los que ya no estan.
        for (const entt::entity handle : world->registry().view<vscript::VisualScript>()) {
            if (h.instances.contains(handle)) continue;
            const ecs::Entity e = world->wrap(handle);
            if (e.activeInHierarchy()) h.createInstance(e, e.get<vscript::VisualScript>());
        }
        for (auto it = h.instances.begin(); it != h.instances.end();) {
            if (!world->registry().valid(it->first) || !world->registry().all_of<vscript::VisualScript>(it->first)) {
                it = h.instances.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const entt::entity handle : h.order()) {
        const auto it = h.instances.find(handle);
        if (it == h.instances.end() || !h.runnable(handle)) continue;
        if (phase == 0) {
            if (!it->second.started) {
                it->second.started = true;
                call(it->second, "Start");
            }
            call(it->second, "Update", dt);
        } else if (it->second.started) {
            call(it->second, "LateUpdate", dt);
        }
    }
}

void ScriptSystem::Impl::vsFixed(float step) {
    if (!vs) return;
    for (const entt::entity handle : vs->order()) {
        const auto it = vs->instances.find(handle);
        if (it == vs->instances.end() || !it->second.started || !vs->runnable(handle)) continue;
        call(it->second, "FixedUpdate", step);
    }
}

void ScriptSystem::Impl::vsStop(bool destroy_events) {
    if (!vs) return;
    if (destroy_events) {
        for (auto& [handle, inst] : vs->instances) call(inst, "OnDestroy");
    }
    vs.reset();
    vs_break.pending = false;
}

ScriptSystem::Impl::Instance* ScriptSystem::Impl::vsInstance(entt::entity handle) {
    if (!vs) return nullptr;
    const auto it = vs->instances.find(handle);
    return it == vs->instances.end() ? nullptr : &it->second;
}

void ScriptSystem::Impl::vsForget(entt::entity handle) {
    if (vs) vs->instances.erase(handle);
}

bool ScriptSystem::Impl::vsReload(const std::string& file) {
    std::string extension = std::filesystem::path(file).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != vscript::kGraphExtension) return false;
    if (!vs) return true;
    std::string error;
    vs->failed.erase(file);
    sol::object cls = vs->load(file, &error);
    if (!cls.is<sol::table>()) {
        vs->failed[file] = error;
        fail(file, error);
        return true;
    }
    sol::table table = cls.as<sol::table>();
    vs->classes[file] = table;
    // Las instancias siguen con sus datos, con los nodos nuevos y lo que les
    // falte (variables nuevas...) puesto por __vsinit.
    for (auto& [handle, inst] : vs->instances) {
        if (inst.file != file) continue;
        sol::table meta = lua->create_table();
        meta["__index"] = table;
        inst.self[sol::metatable_key] = meta;
        inst.failed = false;
        call(inst, "__vsinit");
    }
    write(0, "Visual Script recargado: " + file);
    if (world != nullptr) {
        for (const entt::entity handle : world->registry().view<vscript::VisualScript>()) {
            const ecs::Entity e = world->wrap(handle);
            if (!vs->instances.contains(handle) && e.get<vscript::VisualScript>().graph == file && e.activeInHierarchy()) {
                vs->createInstance(e, e.get<vscript::VisualScript>());
            }
        }
    }
    return true;
}

// --- API publica (depuracion del editor) ---

bool ScriptSystem::visualScriptDebug(ecs::Entity entity, VisualScriptDebug& out) {
    Impl& d = *impl_;
    out = VisualScriptDebug{};
    if (!entity.valid() || !d.lua) return false;
    Impl::Instance* inst = d.vsInstance(entity.handle());
    if (inst == nullptr) return false;
    sol::state& L = *d.lua;
    out.graph = inst->file;
    out.failed = inst->failed;
    sol::object now = L["Time"]["time"];
    if (now.get_type() == sol::type::number) out.now = now.as<double>();
    sol::protected_function tostring_fn = L["tostring"];
    const auto text = [&](const sol::object& o) -> std::string {
        switch (o.get_type()) {
            case sol::type::lua_nil:
            case sol::type::none: return "nil";
            case sol::type::boolean: return o.as<bool>() ? "true" : "false";
            case sol::type::number: {
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%g", o.as<double>());
                return buffer;
            }
            case sol::type::string: return o.as<std::string>();
            default: break;
        }
        if (!tostring_fn.valid()) return "?";
        sol::protected_function_result r = tostring_fn(o);
        if (!r.valid()) return "?";
        sol::object s = r;
        return s.is<std::string>() ? s.as<std::string>() : std::string("?");
    };
    sol::object executed = inst->self["__t"];
    if (executed.is<sol::table>()) {
        executed.as<sol::table>().for_each([&](const sol::object& k, const sol::object& v) {
            if (k.get_type() == sol::type::number && v.get_type() == sol::type::number) out.executed[k.as<int>()] = v.as<double>();
        });
    }
    sol::object values = inst->self["__o"];
    if (values.is<sol::table>()) {
        values.as<sol::table>().for_each([&](const sol::object& k, const sol::object& v) {
            if (k.get_type() == sol::type::string) out.values[k.as<std::string>()] = text(v);
        });
    }
    inst->self.for_each([&](const sol::object& k, const sol::object& v) {
        if (k.get_type() != sol::type::string) return;
        const std::string name = k.as<std::string>();
        if (name == "entity" || name.rfind("__", 0) == 0) return;
        out.variables[name] = text(v);
    });
    return true;
}

void ScriptSystem::setVisualScriptDebugging(bool on) {
    impl_->vs_debugging = on;
    if (impl_->vs) impl_->vs->installBreakpoints();
}

void ScriptSystem::setVisualScriptBreakpoints(const std::string& graph, const std::vector<int>& nodes) {
    impl_->vs_breakpoints[graph] = std::set<int>(nodes.begin(), nodes.end());
    if (impl_->vs) impl_->vs->installBreakpoints();
}

bool ScriptSystem::takeVisualScriptBreak(std::string& graph, int& node, entt::entity& entity) {
    Impl& d = *impl_;
    if (!d.vs_break.pending) return false;
    graph = d.vs_break.graph;
    node = d.vs_break.node;
    entity = d.vs_break.entity;
    d.vs_break.pending = false;
    return true;
}

std::vector<entt::entity> ScriptSystem::visualScriptObjects(const std::string& graph) const {
    std::vector<entt::entity> out;
    const Impl& d = *impl_;
    if (!d.vs) return out;
    for (const auto& [handle, inst] : d.vs->instances) {
        if (graph.empty() || inst.file == graph) out.push_back(handle);
    }
    std::sort(out.begin(), out.end());
    return out;
}
