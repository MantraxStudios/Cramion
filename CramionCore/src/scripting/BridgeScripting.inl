// Puente de la API de Lua para otros lenguajes (los scripts de C++): una
// llamada en JSON se hace en el estado de Lua del juego y el resultado vuelve
// en JSON. Asi los scripts de C++ tienen TODA la API sin escribirla dos veces.
//
// Valores: null, bool, numero, texto, listas, objetos y los especiales
//   {"$v":[x,y,z]} Vec3, {"$q":[x,y,z,w]} Quat, {"$e":id} entidad (handle+1),
//   {"$f":id} funcion del script (callback: al llamarla Lua, va al sumidero),
//   {"$h":id} objeto del motor (malla, maquina de estados, tabla...) que se
//   guarda aqui y se usa luego como `self` de un metodo.
//
// Se incluye al final de Scripting.cpp (necesita ScriptSystem::Impl).

namespace {

std::uint64_t bridgeEntityId(entt::entity h) {
    return h == entt::null ? 0 : static_cast<std::uint64_t>(entt::to_integral(h)) + 1;
}

}  // namespace

nlohmann::json ScriptSystem::Impl::bridgeToJson(const sol::object& o, int depth) {
    using json = nlohmann::json;
    if (depth > 16) return nullptr;
    switch (o.get_type()) {
        case sol::type::lua_nil:
        case sol::type::none: return nullptr;
        case sol::type::boolean: return o.as<bool>();
        case sol::type::number: {
            const double d = o.as<double>();
            return std::isfinite(d) ? json(d) : json(nullptr);
        }
        case sol::type::string: return o.as<std::string>();
        case sol::type::userdata: {
            if (o.is<Vec3>()) {
                const Vec3 v = o.as<Vec3>();
                return json{{"$v", {v.x, v.y, v.z}}};
            }
            if (o.is<core::Quat>()) {
                const core::Quat q = o.as<core::Quat>();
                return json{{"$q", {q.x, q.y, q.z, q.w}}};
            }
            if (o.is<LuaEntity>()) return json{{"$e", bridgeEntityId(o.as<LuaEntity>().handle)}};
            break;
        }
        case sol::type::table: {
            const sol::table t = o.as<sol::table>();
            // Lista (1..n) u objeto.
            const std::size_t n = t.size();
            bool sequence = n > 0;
            std::size_t keys = 0;
            t.for_each([&](const sol::object& k, const sol::object&) {
                ++keys;
                if (k.get_type() != sol::type::number) sequence = false;
            });
            if (sequence && keys == n) {
                json a = json::array();
                for (std::size_t i = 1; i <= n; ++i) a.push_back(bridgeToJson(t[i], depth + 1));
                return a;
            }
            // Una tabla con metodos (una clase de Lua): mejor como objeto del motor.
            bool has_function = false;
            json obj = json::object();
            t.for_each([&](const sol::object& k, const sol::object& v) {
                if (v.get_type() == sol::type::function) has_function = true;
                if (k.get_type() == sol::type::string) obj[k.as<std::string>()] = bridgeToJson(v, depth + 1);
                else if (k.get_type() == sol::type::number) obj[std::to_string(k.as<long long>())] = bridgeToJson(v, depth + 1);
            });
            if (!has_function) return obj;
            break;
        }
        default: break;
    }
    // Lo demas (mallas, maquinas de estados, funciones, tablas con metodos): un handle.
    if (bridge_handles.empty()) bridge_handles.emplace_back(sol::lua_nil);  // 0 = ninguno
    bridge_handles.push_back(o);
    return nlohmann::json{{"$h", bridge_handles.size() - 1}};
}

sol::object ScriptSystem::Impl::bridgeFromJson(const nlohmann::json& j, int depth) {
    sol::state& L = *lua;
    if (depth > 16 || j.is_null()) return sol::lua_nil;
    if (j.is_boolean()) return sol::make_object(L, j.get<bool>());
    if (j.is_number()) return sol::make_object(L, j.get<double>());
    if (j.is_string()) return sol::make_object(L, j.get<std::string>());
    if (j.is_array()) {
        sol::table t = L.create_table(static_cast<int>(j.size()), 0);
        for (std::size_t i = 0; i < j.size(); ++i) t[i + 1] = bridgeFromJson(j[i], depth + 1);
        return t;
    }
    if (j.is_object()) {
        if (j.size() == 1) {
            // Ojo: *j.items().begin() es una referencia a un temporal; con el iterador, no.
            const auto first = j.begin();
            const std::string key = first.key();
            const nlohmann::json& value = first.value();
            if (key == "$v" && value.is_array() && value.size() >= 3) {
                return sol::make_object(L, Vec3{value[0].get<float>(), value[1].get<float>(), value[2].get<float>()});
            }
            if (key == "$q" && value.is_array() && value.size() >= 4) {
                return sol::make_object(L, core::Quat{value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>()});
            }
            if (key == "$e" && value.is_number()) {
                const auto id = value.get<std::uint64_t>();
                const entt::entity h = id == 0 ? entt::null
                                               : static_cast<entt::entity>(static_cast<std::underlying_type_t<entt::entity>>(id - 1));
                return sol::make_object(L, LuaEntity{h, world});
            }
            if (key == "$h" && value.is_number()) {
                const auto id = value.get<std::size_t>();
                return id > 0 && id < bridge_handles.size() ? bridge_handles[id] : sol::object(sol::lua_nil);
            }
            if (key == "$f" && value.is_number()) {
                const auto id = value.get<std::uint64_t>();
                return sol::make_object(L, [this, id](sol::variadic_args va) {
                    nlohmann::json args = nlohmann::json::array();
                    for (auto v : va) args.push_back(bridgeToJson(v, 0));
                    if (bridge_sink) bridge_sink(id, args.dump());
                });
            }
        }
        sol::table t = L.create_table();
        for (const auto& [key, value] : j.items()) t[key] = bridgeFromJson(value, depth + 1);
        return t;
    }
    return sol::lua_nil;
}

// "Audio.playOneShot" -> el objeto de Lua (tablas anidadas desde los globales).
sol::object ScriptSystem::Impl::bridgeResolve(const std::string& path) {
    sol::object current = lua->globals();
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t dot = path.find('.', start);
        const std::string part = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (current.get_type() == sol::type::table) {
            current = current.as<sol::table>()[part];
        } else if (current.get_type() == sol::type::userdata) {
            current = current.as<sol::userdata>()[part];
        } else {
            return sol::lua_nil;
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return current;
}

std::string ScriptSystem::bridgeCall(const std::string& request_text) {
    using json = nlohmann::json;
    Impl& d = *impl_;
    json reply;
    if (!d.lua) return json{{"ok", false}, {"error", "el juego no esta en marcha"}}.dump();
    const json request = json::parse(request_text, nullptr, false);
    if (!request.is_object()) return json{{"ok", false}, {"error", "peticion no valida"}}.dump();
    const std::string op = request.value("op", std::string("call"));
    const std::string fn = request.value("fn", std::string());
    try {
        sol::object self = request.contains("self") ? d.bridgeFromJson(request["self"], 0) : sol::object(sol::lua_nil);
        const bool has_self = request.contains("self");
        if (has_self && self.get_type() == sol::type::lua_nil) {
            return json{{"ok", false}, {"error", "el objeto ya no existe"}}.dump();
        }
        const auto index = [&](const sol::object& target, const std::string& key) -> sol::object {
            if (target.get_type() == sol::type::table) return target.as<sol::table>()[key];
            if (target.get_type() == sol::type::userdata) return target.as<sol::userdata>()[key];
            return sol::lua_nil;
        };
        if (op == "get") {
            const sol::object v = has_self ? index(self, request.value("key", fn)) : d.bridgeResolve(fn);
            reply = json{{"ok", true}, {"result", d.bridgeToJson(v, 0)}};
        } else if (op == "set") {
            const sol::object v = d.bridgeFromJson(request.contains("value") ? request["value"] : json(), 0);
            if (has_self) {
                const std::string key = request.value("key", fn);
                if (self.get_type() == sol::type::table) self.as<sol::table>()[key] = v;
                else if (self.get_type() == sol::type::userdata) self.as<sol::userdata>()[key] = v;
            } else {
                const std::size_t dot = fn.rfind('.');
                const sol::object owner = dot == std::string::npos ? sol::object(d.lua->globals()) : d.bridgeResolve(fn.substr(0, dot));
                const std::string key = dot == std::string::npos ? fn : fn.substr(dot + 1);
                if (owner.get_type() == sol::type::table) owner.as<sol::table>()[key] = v;
                else if (owner.get_type() == sol::type::userdata) owner.as<sol::userdata>()[key] = v;
                else return json{{"ok", false}, {"error", "no existe " + fn}}.dump();
            }
            reply = json{{"ok", true}, {"result", nullptr}};
        } else {
            const sol::object target = has_self ? index(self, fn) : d.bridgeResolve(fn);
            if (target.get_type() != sol::type::function) {
                // Constructores de tipos (Mesh.new en un usertype) y tablas llamables.
                if (target.get_type() == sol::type::lua_nil) return json{{"ok", false}, {"error", "no existe " + fn}}.dump();
            }
            std::vector<sol::object> args;
            if (has_self) args.push_back(self);
            if (request.contains("args") && request["args"].is_array()) {
                for (const json& a : request["args"]) args.push_back(d.bridgeFromJson(a, 0));
            }
            sol::protected_function f = target;
            sol::protected_function_result r = f(sol::as_args(args));
            if (!r.valid()) {
                const sol::error e = r;
                return json{{"ok", false}, {"error", std::string(e.what())}}.dump();
            }
            const int returns = r.return_count();
            json result = nullptr;
            if (returns == 1) {
                result = d.bridgeToJson(r.get<sol::object>(0), 0);
            } else if (returns > 1) {
                result = json::array();
                for (int i = 0; i < returns; ++i) result.push_back(d.bridgeToJson(r.get<sol::object>(i), 0));
            }
            reply = json{{"ok", true}, {"result", result}};
        }
    } catch (const std::exception& e) {
        return json{{"ok", false}, {"error", std::string(e.what())}}.dump();
    }
    return reply.dump();
}

void ScriptSystem::setBridgeCallbackSink(std::function<void(std::uint64_t id, const std::string& args_json)> sink) {
    impl_->bridge_sink = std::move(sink);
}
