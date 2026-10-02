// Partidas, localizacion y dialogos desde Lua (gameplay/SaveGame.h,
// Localization.h, Dialogue.h):
//
//   Save.save("slot1", "Antes del jefe")   -- guarda (se escribe en segundo plano)
//   Save.load("slot1")                    -- carga (cambia de escena si hace falta)
//   Save.list() / Save.info("slot1") / Save.remove("slot1") / Save.exists(...)
//   Save.setValue("llave_roja", true)     -- valores sueltos dentro de la partida
//   Save.setAutosave(120)                 -- autoguardado cada 2 minutos
//   function MiScript:OnSave() return { vida = self.vida } end
//   function MiScript:OnLoad(datos) self.vida = datos.vida end
//
//   Text.get("menu.jugar")  Text.get("hola", nombre)  Text.plural("monedas", n)
//   Text.setLanguage("en")  Text.onLanguageChanged(function(idioma) ... end)
//
//   Dialogue.start("Dialogos/Mercader")   Dialogue.next()   Dialogue.choose(1)
//   Dialogue.onLine(function(linea) ... end)  onChoices / onEvent / onEnd
//
// Se incluye al final de Scripting.cpp (necesita ScriptSystem::Impl).

// Valor de Lua -> JSON de partida: como Json.encode, mas Vec3, Quat y
// entidades (por UUID). Las funciones y las instancias de clases se saltan.
nlohmann::json ScriptSystem::Impl::saveValueToJson(const sol::object& v, int depth) {
    using json = nlohmann::json;
    if (depth > 32) return nullptr;
    switch (v.get_type()) {
        case sol::type::boolean: return v.as<bool>();
        case sol::type::number: {
            const double d = v.as<double>();
            if (!std::isfinite(d)) return nullptr;
            if (std::floor(d) == d && std::abs(d) < 9007199254740992.0) return static_cast<std::int64_t>(d);
            return d;
        }
        case sol::type::string: return v.as<std::string>();
        case sol::type::userdata:
            if (v.is<Vec3>()) {
                const Vec3 p = v.as<Vec3>();
                return json{{"$v", {p.x, p.y, p.z}}};
            }
            if (v.is<core::Quat>()) {
                const core::Quat q = v.as<core::Quat>();
                return json{{"$q", {q.x, q.y, q.z, q.w}}};
            }
            if (v.is<LuaEntity>()) {
                const ecs::Entity e = v.as<LuaEntity>().get();
                return e.valid() ? json{{"$ent", e.uuid().toString()}} : json(nullptr);
            }
            return nullptr;
        case sol::type::table: {
            const sol::table t = v.as<sol::table>();
            // Una instancia de script u objeto con metatabla: no es un dato.
            if (t[sol::metatable_key].get_type() == sol::type::table) return nullptr;
            const std::size_t n = t.size();
            std::size_t keys = 0;
            bool sequence = n > 0;
            t.for_each([&](const sol::object& k, const sol::object&) {
                ++keys;
                if (k.get_type() != sol::type::number) sequence = false;
            });
            if (sequence && keys == n) {
                json a = json::array();
                for (std::size_t i = 1; i <= n; ++i) a.push_back(saveValueToJson(t[i], depth + 1));
                return a;
            }
            json o = json::object();
            t.for_each([&](const sol::object& k, const sol::object& value) {
                const sol::type vt = value.get_type();
                if (vt == sol::type::function || vt == sol::type::thread) return;
                std::string key;
                if (k.get_type() == sol::type::string) {
                    key = k.as<std::string>();
                } else if (k.get_type() == sol::type::number) {
                    const double d = k.as<double>();
                    key = "#" + (std::floor(d) == d ? std::to_string(static_cast<long long>(d)) : std::to_string(d));
                } else {
                    return;
                }
                o[key] = saveValueToJson(value, depth + 1);
            });
            return o;
        }
        default: return nullptr;
    }
}

sol::object ScriptSystem::Impl::saveValueFromJson(const nlohmann::json& j) {
    sol::state_view L(*lua);
    if (j.is_object()) {
        if (const auto v = j.find("$v"); v != j.end() && v->is_array() && v->size() == 3 && j.size() == 1) {
            return sol::make_object(L, Vec3{(*v)[0].get<float>(), (*v)[1].get<float>(), (*v)[2].get<float>()});
        }
        if (const auto q = j.find("$q"); q != j.end() && q->is_array() && q->size() == 4 && j.size() == 1) {
            return sol::make_object(L, core::Quat{(*q)[0].get<float>(), (*q)[1].get<float>(), (*q)[2].get<float>(), (*q)[3].get<float>()});
        }
        if (const auto e = j.find("$ent"); e != j.end() && e->is_string() && j.size() == 1) {
            if (world == nullptr) return sol::lua_nil;
            const ecs::Entity found = world->find(Uuid::parse(e->get<std::string>()));
            return found.valid() ? sol::make_object(L, LuaEntity{found.handle(), world}) : sol::object(sol::lua_nil);
        }
        sol::table t = L.create_table();
        for (const auto& [key, item] : j.items()) {
            if (key.size() > 1 && key[0] == '#') {
                // Clave numerica de una tabla mixta.
                char* end = nullptr;
                const double d = std::strtod(key.c_str() + 1, &end);
                if (end != nullptr && *end == '\0') {
                    t[d] = saveValueFromJson(item);
                    continue;
                }
            }
            t[key] = saveValueFromJson(item);
        }
        return t;
    }
    if (j.is_array()) {
        sol::table t = L.create_table(static_cast<int>(j.size()), 0);
        int i = 1;
        for (const auto& item : j) t[i++] = saveValueFromJson(item);
        return t;
    }
    if (j.is_boolean()) return sol::make_object(L, j.get<bool>());
    if (j.is_number_integer()) return sol::make_object(L, static_cast<lua_Integer>(j.get<std::int64_t>()));
    if (j.is_number()) return sol::make_object(L, j.get<double>());
    if (j.is_string()) return sol::make_object(L, j.get<std::string>());
    return sol::lua_nil;
}

// Estado del script de un objeto: sus propiedades y lo que devuelva OnSave().
nlohmann::json ScriptSystem::Impl::captureScriptState(ecs::Entity e) {
    using json = nlohmann::json;
    const auto it = instances.find(e.handle());
    if (it == instances.end() || lua == nullptr) return nullptr;
    Instance& inst = it->second;
    std::set<std::string> names;
    if (sol::table* cls = classFor(inst.file)) {
        sol::optional<sol::table> defaults = (*cls)["properties"];
        if (defaults) {
            for (const auto& [k, v] : *defaults) {
                if (k.get_type() == sol::type::string) names.insert(k.as<std::string>());
            }
        }
    }
    if (const Script* script = e.tryGet<Script>()) {
        for (const ScriptProperty& p : script->properties) {
            if (!p.name.empty()) names.insert(p.name);
        }
    }
    json out = json::object();
    json props = json::object();
    for (const std::string& name : names) props[name] = saveValueToJson(inst.self[name]);
    if (!props.empty()) out["props"] = std::move(props);
    if (!inst.failed) {
        sol::object fn = inst.self["OnSave"];
        if (fn.get_type() == sol::type::function) {
            sol::protected_function pf = fn;
            sol::protected_function_result r = pf(inst.self);
            if (!r.valid()) {
                sol::error err = r;
                fail(inst.file, err.what());
            } else if (r.return_count() > 0) {
                out["data"] = saveValueToJson(r.get<sol::object>());
            }
        }
    }
    return out;
}

void ScriptSystem::Impl::restoreScriptState(ecs::Entity e, const nlohmann::json& state) {
    const auto it = instances.find(e.handle());
    if (it == instances.end() || lua == nullptr || !state.is_object()) return;
    Instance& inst = it->second;
    if (const auto props = state.find("props"); props != state.end() && props->is_object()) {
        for (const auto& [name, value] : props->items()) inst.self[name] = saveValueFromJson(value);
    }
    const auto data = state.find("data");
    sol::object payload = data != state.end() ? saveValueFromJson(*data) : sol::object(sol::lua_nil);
    call(inst, "OnLoad", payload);
}

// "Dialogos/Mercader", "Mercader" o "Mercader.crdialog" -> archivo en Assets.
std::filesystem::path ScriptSystem::Impl::findDialogue(const std::string& name) const {
    namespace fs = std::filesystem;
    if (name.empty() || root.empty()) return {};
    fs::path direct = root / fromUtf8(name);
    if (direct.extension() != gameplay::kDialogueExtension) direct += gameplay::kDialogueExtension;
    std::error_code e;
    if (fs::is_regular_file(direct, e)) return direct;
    const std::string wanted = lower(fromUtf8(name).stem().string());
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, e);
         !e && it != fs::recursive_directory_iterator(); it.increment(e)) {
        if (it->path().extension() != gameplay::kDialogueExtension) continue;
        if (lower(it->path().stem().string()) == wanted) return it->path();
    }
    return {};
}

void ScriptSystem::Impl::callLuaList(std::vector<sol::protected_function>& list, const char* what,
                                     const std::function<sol::protected_function_result(sol::protected_function&)>& invoke) {
    // Copia: una funcion puede registrar otra.
    std::vector<sol::protected_function> copy = list;
    for (sol::protected_function& fn : copy) {
        if (!fn.valid()) continue;
        sol::protected_function_result r = invoke(fn);
        if (!r.valid()) {
            sol::error err = r;
            fail(what, err.what());
        }
    }
}

sol::table ScriptSystem::Impl::dialogueLineTable(const gameplay::DialogueLine& line) {
    sol::table t = lua->create_table();
    t["speaker"] = line.speaker;
    t["text"] = line.text;
    t["audio"] = line.audio;
    t["node"] = line.node;
    t["autoAdvance"] = line.auto_advance;
    return t;
}

sol::table ScriptSystem::Impl::dialogueChoicesTable(const std::vector<gameplay::DialogueChoice>& choices) {
    sol::table t = lua->create_table();
    int i = 1;
    for (const gameplay::DialogueChoice& c : choices) {
        sol::table item = lua->create_table();
        item["index"] = c.index + 1;  // como en Dialogue.choose (desde 1)
        item["text"] = c.text;
        item["enabled"] = c.enabled;
        t[i++] = item;
    }
    return t;
}

void ScriptSystem::Impl::bindGameplay(sol::state& L) {
    // --- Save ---
    sol::table sv = L.create_named_table("Save");
    sv["save"] = [this](const std::string& slot, sol::optional<std::string> label) {
        if (world == nullptr) return false;
        std::string error;
        if (!saves.save(*world, slot, label.value_or(std::string()), &error)) {
            write(2, "Save.save: " + error);
            return false;
        }
        return true;
    };
    sv["load"] = [this](const std::string& slot) {
        nlohmann::json snapshot;
        std::string error;
        if (!saves.read(slot, snapshot, &error)) {
            write(2, "Save.load(\"" + slot + "\"): " + error);
            return false;
        }
        const std::string scene = snapshot.value("scene", std::string());
        if (!scene.empty() && lower(scene) != lower(scene_name)) {
            // Otra escena: se carga y la partida se aplica al empezar.
            const std::filesystem::path path = findScene(scene);
            if (path.empty()) {
                write(2, "Save.load: no existe la escena \"" + scene + "\" de la partida");
                return false;
            }
            saves.pending_load = std::move(snapshot);
            scene_request = path;
            save_loaded_slot = slot;
            return true;
        }
        // La misma escena: al principio del siguiente frame (no a mitad de los Update).
        save_apply = std::move(snapshot);
        save_loaded_slot = slot;
        return true;
    };
    sv["remove"] = [this](const std::string& slot) { return saves.remove(slot); };
    sv["delete"] = [this](const std::string& slot) { return saves.remove(slot); };
    sv["exists"] = [this](const std::string& slot) { return saves.exists(slot); };
    const auto info_table = [this](const gameplay::SaveSlotInfo& i) {
        sol::table t = lua->create_table();
        t["slot"] = i.slot;
        t["label"] = i.label;
        t["scene"] = i.scene;
        t["date"] = i.date;
        t["time"] = static_cast<double>(i.unix_time);
        t["playtime"] = i.playtime;
        t["size"] = static_cast<double>(i.size_bytes);
        return t;
    };
    sv["list"] = [this, info_table]() {
        sol::table out = lua->create_table();
        int n = 1;
        for (const gameplay::SaveSlotInfo& i : saves.list()) out[n++] = info_table(i);
        return out;
    };
    sv["info"] = [this, info_table](const std::string& slot) -> sol::object {
        const auto i = saves.info(slot);
        if (!i) return sol::lua_nil;
        return info_table(*i);
    };
    sv["setValue"] = [this](const std::string& key, sol::object value) {
        if (value.get_type() == sol::type::lua_nil || value.get_type() == sol::type::none) {
            saves.values().erase(key);
        } else {
            saves.values()[key] = saveValueToJson(value);
        }
    };
    sv["getValue"] = [this](const std::string& key, sol::object fallback) -> sol::object {
        const auto it = saves.values().find(key);
        if (it == saves.values().end()) return fallback;
        return saveValueFromJson(*it);
    };
    sv["hasValue"] = [this](const std::string& key) { return saves.values().contains(key); };
    sv["deleteValue"] = [this](const std::string& key) { saves.values().erase(key); };
    sv["clearValues"] = [this]() { saves.values() = nlohmann::json::object(); };
    sv["setAutosave"] = [this](float seconds, sol::optional<std::string> slot) { saves.setAutosave(seconds, slot.value_or("autosave")); };
    sv["playtime"] = [this]() { return saves.playtime(); };
    sv["setCompression"] = [this](bool on) { saves.setCompression(on); };
    sv["isWriting"] = [this]() { return saves.writing(); };
    sv["folder"] = [this]() {
        const std::u8string s = saves.folder().u8string();
        return std::string(s.begin(), s.end());
    };
    sv["onLoaded"] = [this](sol::protected_function fn) { save_on_loaded.push_back(std::move(fn)); };

    // --- Text (localizacion) ---
    const auto collect_args = [](sol::variadic_args va, std::vector<std::string>& args, std::map<std::string, std::string>& named) {
        for (const sol::object& a : va) {
            if (a.get_type() == sol::type::table) {
                for (const auto& [k, v] : a.as<sol::table>()) {
                    if (k.get_type() != sol::type::string) continue;
                    sol::state_view lv(a.lua_state());
                    sol::protected_function tostring = lv["tostring"];
                    sol::protected_function_result s = tostring(v);
                    named[k.as<std::string>()] = s.valid() ? s.get<std::string>() : std::string();
                }
                continue;
            }
            sol::state_view lv(a.lua_state());
            sol::protected_function tostring = lv["tostring"];
            sol::protected_function_result s = tostring(a);
            args.push_back(s.valid() ? s.get<std::string>() : std::string());
        }
    };
    sol::table tx = L.create_named_table("Text");
    tx["get"] = [collect_args](const std::string& key, sol::variadic_args va) {
        std::vector<std::string> args;
        std::map<std::string, std::string> named;
        collect_args(va, args, named);
        return gameplay::localization().get(key, args, named);
    };
    tx["plural"] = [collect_args](const std::string& key, double count, sol::variadic_args va) {
        std::vector<std::string> args;
        std::map<std::string, std::string> named;
        collect_args(va, args, named);
        return gameplay::localization().plural(key, count, args, named);
    };
    tx["format"] = [collect_args](const std::string& text, sol::variadic_args va) {
        std::vector<std::string> args;
        std::map<std::string, std::string> named;
        collect_args(va, args, named);
        return gameplay::Localization::format(text, args, named);
    };
    tx["has"] = [](const std::string& key) { return gameplay::localization().has(key); };
    tx["language"] = []() { return gameplay::localization().language(); };
    tx["setLanguage"] = [this](const std::string& code) {
        if (!gameplay::localization().setLanguage(code)) {
            write(1, "Text.setLanguage: el proyecto no tiene el idioma \"" + code + "\"");
            return false;
        }
        return true;
    };
    tx["languages"] = [this]() {
        sol::table out = lua->create_table();
        int n = 1;
        for (const gameplay::LocalizationLanguage& l : gameplay::localization().languages) {
            sol::table item = lua->create_table();
            item["code"] = l.code;
            item["name"] = l.name;
            out[n++] = item;
        }
        return out;
    };
    tx["systemLanguage"] = []() { return gameplay::Localization::systemLanguage(); };
    tx["onLanguageChanged"] = [this](sol::protected_function fn) {
        const int id = gameplay::localization().addListener([this, fn](const std::string& language) {
            sol::protected_function f = fn;
            sol::protected_function_result r = f(language);
            if (!r.valid()) {
                sol::error err = r;
                fail("Text.onLanguageChanged", err.what());
            }
        });
        language_listeners.push_back(id);
        return id;
    };
    tx["removeListener"] = [this](int id) {
        gameplay::localization().removeListener(id);
        language_listeners.erase(std::remove(language_listeners.begin(), language_listeners.end(), id), language_listeners.end());
    };

    // --- Dialogue ---
    sol::table dl = L.create_named_table("Dialogue");
    dl["start"] = [this](const std::string& name) {
        std::string error;
        if (!dialogue.start(name, &error)) {
            write(2, "Dialogue.start(\"" + name + "\"): " + (error.empty() ? std::string("no existe el dialogo") : error));
            return false;
        }
        return true;
    };
    dl["stop"] = [this]() { dialogue.stop(); };
    dl["next"] = [this]() { dialogue.advance(); };
    dl["advance"] = [this]() { dialogue.advance(); };
    dl["choose"] = [this](int index) { return dialogue.choose(index - 1); };
    dl["isActive"] = [this]() { return dialogue.active(); };
    dl["isWaitingChoice"] = [this]() { return dialogue.waitingChoice(); };
    dl["name"] = [this]() { return dialogue.active() ? dialogue.dialogueName() : std::string(); };
    dl["currentLine"] = [this]() -> sol::object {
        if (!dialogue.hasLine()) return sol::lua_nil;
        return dialogueLineTable(dialogue.line());
    };
    dl["choices"] = [this]() { return dialogueChoicesTable(dialogue.choices()); };
    dl["setVar"] = [this](const std::string& name, sol::object value) { dialogue.setVariable(name, saveValueToJson(value)); };
    dl["getVar"] = [this](const std::string& name) { return saveValueFromJson(dialogue.variable(name)); };
    dl["setAutoAudio"] = [this](bool on) { dialogue_auto_audio = on; };
    dl["onStart"] = [this](sol::protected_function fn) { dialogue_on_start.push_back(std::move(fn)); };
    dl["onLine"] = [this](sol::protected_function fn) { dialogue_on_line.push_back(std::move(fn)); };
    dl["onChoices"] = [this](sol::protected_function fn) { dialogue_on_choices.push_back(std::move(fn)); };
    dl["onEvent"] = [this](sol::protected_function fn) { dialogue_on_event.push_back(std::move(fn)); };
    dl["onEnd"] = [this](sol::protected_function fn) { dialogue_on_end.push_back(std::move(fn)); };
}

void ScriptSystem::Impl::gameplayStart() {
    gameplay::setActiveDialogue(&dialogue);
    dialogue.setResolver([this](const std::string& name) { return findDialogue(name); });
    saves.setPhysics(physics);
    saves.setScriptHooks(gameplay::SaveSystem::ScriptHooks{
        [this](ecs::Entity e) { return captureScriptState(e); },
        [this](ecs::Entity e, const nlohmann::json& state) { restoreScriptState(e, state); }});
    saves.destroy = [this](ecs::Entity e) { pending_destroy.push_back(e.handle()); };
    saves.spawned = [this](ecs::Entity root_entity) {
        // Sus scripts ya (OnLoad los necesita).
        std::vector<entt::entity> stack{root_entity.handle()};
        while (!stack.empty()) {
            const entt::entity h = stack.back();
            stack.pop_back();
            const ecs::Entity e = world->wrap(h);
            if (const Script* s = e.tryGet<Script>(); s != nullptr && !instances.contains(h) && e.activeInHierarchy()) createInstance(e, *s);
            for (const entt::entity c : e.children()) stack.push_back(c);
        }
    };
    saves.extra_capture = [this]() { return nlohmann::json{{"dialogue", dialogue.variables()}}; };
    saves.extra_restore = [this](const nlohmann::json& extra) {
        if (extra.is_object() && extra.contains("dialogue") && extra["dialogue"].is_object()) dialogue.variables() = extra["dialogue"];
    };
    if (world == nullptr) return;
    saves.beginScene(*world, scene_name, "");
    // Partida de otra escena (Save.load): ahora que la escena esta cargada.
    if (saves.pending_load) {
        save_apply = std::move(*saves.pending_load);
        saves.pending_load.reset();
        applyPendingSave();
    }
}

void ScriptSystem::Impl::applyPendingSave() {
    if (!save_apply || world == nullptr) return;
    nlohmann::json snapshot = std::move(*save_apply);
    save_apply.reset();
    std::string error;
    if (!saves.apply(*world, snapshot, &error)) {
        write(2, "Save.load: " + error);
        return;
    }
    flushDestroys();
    save_notify_loaded = true;
}

void ScriptSystem::Impl::gameplayUpdate(float dt) {
    if (world == nullptr) return;
    applyPendingSave();
    if (save_notify_loaded) {
        save_notify_loaded = false;
        const std::string slot = save_loaded_slot;
        callLuaList(save_on_loaded, "Save.onLoaded", [&](sol::protected_function& fn) { return fn(slot); });
    }
    saves.update(*world, dt);
    if (const std::string error = saves.takeWriteError(); !error.empty()) write(2, "Save: " + error);

    dialogue.update(dt);
    for (const gameplay::DialogueEvent& ev : dialogue.takeEvents()) {
        switch (ev.kind) {
            case gameplay::DialogueEvent::Kind::Started:
                callLuaList(dialogue_on_start, "Dialogue.onStart", [&](sol::protected_function& fn) { return fn(ev.dialogue); });
                break;
            case gameplay::DialogueEvent::Kind::Line: {
                if (dialogue_auto_audio && !ev.line.audio.empty() && audio != nullptr) {
                    core::Vec3 at{};
                    audio->playOneShot(ev.line.audio, at, 1.0f, false);
                }
                sol::table line = dialogueLineTable(ev.line);
                callLuaList(dialogue_on_line, "Dialogue.onLine", [&](sol::protected_function& fn) { return fn(line); });
                break;
            }
            case gameplay::DialogueEvent::Kind::Choices: {
                sol::table choices = dialogueChoicesTable(ev.choices);
                callLuaList(dialogue_on_choices, "Dialogue.onChoices", [&](sol::protected_function& fn) { return fn(choices); });
                break;
            }
            case gameplay::DialogueEvent::Kind::Event: {
                callLuaList(dialogue_on_event, "Dialogue.onEvent",
                            [&](sol::protected_function& fn) { return fn(ev.name, ev.argument); });
                // Metodo del script de un objeto de la escena.
                if (!ev.method.empty()) {
                    const ecs::Entity target = ev.target.empty() ? ecs::Entity{} : world->findByName(ev.target);
                    if (!target.valid()) {
                        write(1, "Dialogo: no hay un objeto \"" + ev.target + "\" para el evento " + ev.name);
                    } else if (const auto it = instances.find(target.handle()); it != instances.end()) {
                        call(it->second, ev.method.c_str(), ev.argument);
                    }
                }
                break;
            }
            case gameplay::DialogueEvent::Kind::Ended:
                callLuaList(dialogue_on_end, "Dialogue.onEnd", [&](sol::protected_function& fn) { return fn(ev.dialogue); });
                break;
        }
    }
}

void ScriptSystem::Impl::gameplayStop() {
    dialogue.stop();
    dialogue.takeEvents();  // nadie los va a oir
    if (gameplay::activeDialogue() == &dialogue) gameplay::setActiveDialogue(nullptr);
    for (const int id : language_listeners) gameplay::localization().removeListener(id);
    language_listeners.clear();
    dialogue_on_start.clear();
    dialogue_on_line.clear();
    dialogue_on_choices.clear();
    dialogue_on_event.clear();
    dialogue_on_end.clear();
    save_on_loaded.clear();
    save_apply.reset();  // una carga de la misma escena a medias se pierde al parar
    // Las partidas pendientes de escribir, al disco ya (el editor puede salir de Play y cerrar).
    saves.flush();
    saves.setScriptHooks({});
    saves.destroy = nullptr;
    saves.spawned = nullptr;
}
