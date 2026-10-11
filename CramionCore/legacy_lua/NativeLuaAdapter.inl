// Adaptador temporal: publica la API nativa (NativeApi.h) en el estado de Lua,
// para que los scripts .lua sigan funcionando mientras la API se migra tabla a
// tabla. Se borra con Lua (fase 9 de PLAN-SIN-LUA.md).
//
// Se incluye al final de Scripting.cpp (necesita ScriptSystem::Impl).

namespace {

// Un objeto del motor de la API nativa visto desde Lua: sus metodos y
// propiedades se buscan por su typeName().
struct LuaNativeHandle {
    std::shared_ptr<api::Handle> handle;
};

}  // namespace

api::Value ScriptSystem::Impl::luaToNative(const sol::object& o, int depth) {
    if (depth > 16) return {};
    switch (o.get_type()) {
        case sol::type::lua_nil:
        case sol::type::none: return {};
        case sol::type::boolean: return api::Value(o.as<bool>());
        case sol::type::number: return api::Value(o.as<double>());
        case sol::type::string: return api::Value(o.as<std::string>());
        case sol::type::userdata: {
            if (o.is<Vec3>()) return api::Value(o.as<Vec3>());
            if (o.is<core::Quat>()) return api::Value(o.as<core::Quat>());
            if (o.is<LuaEntity>()) return api::Value::entity(o.as<LuaEntity>().handle);
            if (o.is<LuaNativeHandle>()) return api::Value::handle(o.as<LuaNativeHandle>().handle);
            return {};
        }
        case sol::type::function: {
            const std::uint64_t id = next_lua_callback++;
            lua_callbacks.emplace(id, o.as<sol::protected_function>());
            return api::Value::function(id);
        }
        case sol::type::table: {
            const sol::table t = o.as<sol::table>();
            const std::size_t n = t.size();
            std::size_t keys = 0;
            bool sequence = n > 0;
            t.for_each([&](const sol::object& k, const sol::object&) {
                ++keys;
                if (k.get_type() != sol::type::number) sequence = false;
            });
            if (sequence && keys == n) {
                api::Value::Array list;
                list.reserve(n);
                for (std::size_t i = 1; i <= n; ++i) list.push_back(luaToNative(t[i], depth + 1));
                return api::Value(std::move(list));
            }
            api::Value obj = api::Value::object();
            t.for_each([&](const sol::object& k, const sol::object& v) {
                if (k.get_type() == sol::type::string) obj.set(k.as<std::string>(), luaToNative(v, depth + 1));
                else if (k.get_type() == sol::type::number) obj.set(std::to_string(k.as<long long>()), luaToNative(v, depth + 1));
            });
            return obj;
        }
        default: return {};
    }
}

sol::object ScriptSystem::Impl::nativeToLua(sol::state_view L, const api::Value& v, int depth) {
    if (depth > 16) return sol::lua_nil;
    switch (v.type()) {
        case api::Value::Type::Nil: return sol::lua_nil;
        case api::Value::Type::Bool: return sol::make_object(L, v.truthy());
        case api::Value::Type::Number: return sol::make_object(L, v.asNumber());
        case api::Value::Type::String: return sol::make_object(L, v.asString());
        case api::Value::Type::Vec3: return sol::make_object(L, v.asVec3());
        case api::Value::Type::Quat: return sol::make_object(L, v.asQuat());
        case api::Value::Type::Entity: return sol::make_object(L, LuaEntity{v.asEntity(), world});
        case api::Value::Type::Handle: return sol::make_object(L, LuaNativeHandle{v.asHandle()});
        case api::Value::Type::Function: {
            // Un callback de un script de C++ que vuelve a Lua: llamarlo lo manda a su script.
            const api::Value fn = v;
            return sol::make_object(L, [this, fn](sol::variadic_args va) {
                api::Value::Array args;
                for (auto a : va) args.push_back(luaToNative(a, 0));
                native.invoke(fn, args);
            });
        }
        case api::Value::Type::Array: {
            sol::table t = L.create_table(static_cast<int>(v.size()), 0);
            for (std::size_t i = 0; i < v.size(); ++i) t[i + 1] = nativeToLua(L, v[i], depth + 1);
            return t;
        }
        case api::Value::Type::Object: {
            sol::table t = L.create_table(0, static_cast<int>(v.size()));
            for (const auto& [k, item] : v.fields()) t[k] = nativeToLua(L, item, depth + 1);
            return t;
        }
    }
    return sol::lua_nil;
}

sol::object ScriptSystem::Impl::callNative(const api::Entry& entry, const api::Value& self, sol::variadic_args va,
                                           std::size_t skip) {
    api::Value::Array args;
    args.reserve(va.size());
    std::size_t i = 0;
    for (auto a : va) {
        if (i++ < skip) continue;
        args.push_back(luaToNative(a, 0));
    }
    api::Call call(self, args);
    try {
        return nativeToLua(sol::state_view(va.lua_state()), entry.call(call), 0);
    } catch (const std::exception& e) {
        // SOL_ALL_SAFETIES_ON: la excepcion llega al script como error de Lua.
        throw sol::error(entry.owner + (entry.member() ? ":" : ".") + entry.name + ": " + e.what());
    }
}

void ScriptSystem::Impl::publishNative(sol::state& L, sol::usertype<LuaEntity>& entity) {
    // Las funciones de Lua que se dieron a la API nativa (UI.onClick...).
    native.setLocalInvoker([this](std::uint64_t id, const api::Value::Array& args) {
        const auto found = lua_callbacks.find(id);
        if (found == lua_callbacks.end() || !lua) return;
        sol::state_view L(found->second.lua_state());
        std::vector<sol::object> list;
        list.reserve(args.size());
        for (const api::Value& a : args) list.push_back(nativeToLua(L, a, 0));
        sol::protected_function_result r = found->second(sol::as_args(list));
        if (!r.valid()) {
            const sol::error e = r;
            write(2, e.what());
        }
    });

    // Objetos del motor: metodos y propiedades por su tipo.
    L.new_usertype<LuaNativeHandle>(
        "NativeObject", sol::no_constructor,
        sol::meta_function::index,
        [this](const LuaNativeHandle& h, const std::string& key, sol::this_state s) -> sol::object {
            const api::Entry* e = h.handle ? native.find(std::string(h.handle->typeName()) + ":" + key) : nullptr;
            if (e == nullptr) return sol::lua_nil;
            const api::Value self = api::Value::handle(h.handle);
            if (e->kind == api::Entry::Kind::Property) {
                const api::Value::Array no_args;
                api::Call call(self, no_args);
                return nativeToLua(sol::state_view(s), e->call(call), 0);
            }
            // obj:metodo(...): el primer argumento es el propio objeto.
            return sol::make_object(s, [this, e, self](sol::variadic_args va) { return callNative(*e, self, va, 1); });
        },
        sol::meta_function::new_index,
        [this](const LuaNativeHandle& h, const std::string& key, const sol::object& value) {
            const api::Entry* e = h.handle ? native.find(std::string(h.handle->typeName()) + ":" + key) : nullptr;
            if (e == nullptr || e->kind != api::Entry::Kind::Property || !e->assign) {
                throw sol::error("no se puede cambiar " + key);
            }
            const api::Value::Array args{luaToNative(value, 0)};
            api::Call call(api::Value::handle(h.handle), args);
            e->assign(call);
        },
        sol::meta_function::to_string,
        [](const LuaNativeHandle& h) { return h.handle ? std::string(h.handle->typeName()) : std::string("nil"); });

    // La tabla (anidada) de una ruta: "Graphics.post" -> Graphics.post.
    const auto tableFor = [&L](const std::string& owner) {
        sol::table t = L.globals();
        std::size_t start = 0;
        while (!owner.empty() && start <= owner.size()) {
            const std::size_t dot = owner.find('.', start);
            const std::string part = owner.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            sol::object next = t[part];
            if (next.get_type() != sol::type::table) {
                t[part] = L.create_table();
                next = t[part];
            }
            t = next.as<sol::table>();
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return t;
    };

    for (const api::Entry& e : native.entries()) {
        const api::Entry* entry = &e;  // la API no cambia mientras hay Lua
        if (e.owner == "Entity" && e.member()) {
            if (e.kind == api::Entry::Kind::Method) {
                entity[e.name] = [this, entry](const LuaEntity& self, sol::variadic_args va) {
                    return callNative(*entry, api::Value::entity(self.handle), va, 1);
                };
            } else if (e.assign) {
                entity[e.name] = sol::property(
                    [this, entry](const LuaEntity& self, sol::this_state s) {
                        const api::Value::Array no_args;
                        api::Call call(api::Value::entity(self.handle), no_args);
                        return nativeToLua(sol::state_view(s), entry->call(call), 0);
                    },
                    [this, entry](const LuaEntity& self, const sol::object& value) {
                        const api::Value::Array args{luaToNative(value, 0)};
                        api::Call call(api::Value::entity(self.handle), args);
                        entry->assign(call);
                    });
            } else {
                entity[e.name] = sol::readonly_property([this, entry](const LuaEntity& self, sol::this_state s) {
                    const api::Value::Array no_args;
                    api::Call call(api::Value::entity(self.handle), no_args);
                    return nativeToLua(sol::state_view(s), entry->call(call), 0);
                });
            }
            continue;
        }
        if (e.member()) continue;  // los de los handles van por LuaNativeHandle
        sol::table t = tableFor(e.owner);
        if (e.kind == api::Entry::Kind::Function) {
            t[e.name] = [this, entry](sol::variadic_args va) { return callNative(*entry, api::Value::nil(), va, 0); };
            continue;
        }
        // Propiedades de una tabla: por su metatabla (sin valor guardado en la tabla).
        sol::table meta = t[sol::metatable_key].get_or_create<sol::table>();
        const std::string owner = e.owner;
        meta["__index"] = [this, owner](const sol::table&, const std::string& key, sol::this_state s) -> sol::object {
            const api::Entry* p = native.find(owner + "." + key);
            if (p == nullptr || p->kind != api::Entry::Kind::Property) return sol::lua_nil;
            const api::Value::Array no_args;
            api::Call call(api::Value::nil(), no_args);
            try {
                return nativeToLua(sol::state_view(s), p->call(call), 0);
            } catch (const std::exception& ex) {
                throw sol::error(owner + "." + key + ": " + ex.what());
            }
        };
        meta["__newindex"] = [this, owner](sol::table self, const std::string& key, const sol::object& value) {
            const api::Entry* p = native.find(owner + "." + key);
            if (p == nullptr || p->kind != api::Entry::Kind::Property) {
                self.raw_set(key, value);
                return;
            }
            if (!p->assign) throw sol::error(owner + "." + key + " es de solo lectura");
            const api::Value::Array args{luaToNative(value, 0)};
            api::Call call(api::Value::nil(), args);
            try {
                p->assign(call);
            } catch (const std::exception& ex) {
                throw sol::error(owner + "." + key + ": " + ex.what());
            }
        };
    }
}
