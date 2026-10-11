// Partidas, localizacion y dialogos (gameplay/SaveGame.h, Localization.h,
// Dialogue.h):
//
//   Save.save("slot1", "Antes del jefe")   guarda (se escribe en segundo plano)
//   Save.load("slot1")                    carga (cambia de escena si hace falta)
//   Save.list() / Save.info("slot1") / Save.remove("slot1") / Save.exists(...)
//   Save.setValue("llave_roja", true)     valores sueltos dentro de la partida
//   Save.setAutosave(120)                 autoguardado cada 2 minutos
//
//   Text.get("menu.jugar")  Text.get("hola", nombre)  Text.plural("monedas", n)
//   Text.setLanguage("en")  Text.onLanguageChanged(funcion)
//
//   Dialogue.start("Dialogos/Mercader")   Dialogue.next()   Dialogue.choose(1)
//   Dialogue.onLine(funcion)  onChoices / onEvent / onEnd
//
// Los scripts de C++ de los objetos Saveable reciben los mensajes "OnSave"
// (al guardar) y "OnLoad" (al cargar) en onMessage: su estado vive en el
// proceso de los scripts, asi que lo que quieran guardar va en valores
// sueltos (Save.setValue), que entran en cada partida. Los callbacks llegan
// al script un poco despues (no devuelven nada).

#include "Modules.h"

#include "CramionCore/audio/Audio.h"
#include "CramionCore/gameplay/Localization.h"
#include "CramionCore/scripting/CppScripts.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>

namespace cramion::scripting::native {
namespace {

using json = nlohmann::json;

struct GameplayState {
    // Save.load de la misma escena: se aplica al principio del siguiente frame.
    std::optional<json> save_apply;
    bool save_notify_loaded = false;
    std::string save_loaded_slot;
    bool dialogue_auto_audio = true;
    // Callbacks de los scripts (se olvidan al parar).
    std::vector<api::Value> save_on_loaded;
    std::vector<api::Value> dialogue_on_start, dialogue_on_line, dialogue_on_choices, dialogue_on_event, dialogue_on_end;
    std::vector<int> language_listeners;
};

// Valor de la API -> JSON de partida: como Json.encode, mas Vec3, Quat y
// entidades (por UUID). Las funciones y los objetos del motor se saltan.
json saveValueToJson(const Runtime& rt, const api::Value& v, int depth = 0) {
    if (depth > 32) return nullptr;
    switch (v.type()) {
        case api::Value::Type::Bool: return v.truthy();
        case api::Value::Type::Number: {
            const double d = v.asNumber();
            if (!std::isfinite(d)) return nullptr;
            if (std::floor(d) == d && std::abs(d) < 9007199254740992.0) return static_cast<std::int64_t>(d);
            return d;
        }
        case api::Value::Type::String: return v.asString();
        case api::Value::Type::Vec3: {
            const core::Vec3 p = v.asVec3();
            return json{{"$v", {p.x, p.y, p.z}}};
        }
        case api::Value::Type::Quat: {
            const core::Quat q = v.asQuat();
            return json{{"$q", {q.x, q.y, q.z, q.w}}};
        }
        case api::Value::Type::Entity: {
            const ecs::Entity e = rt.entity(v.asEntity());
            return e.valid() ? json{{"$ent", e.uuid().toString()}} : json(nullptr);
        }
        case api::Value::Type::Array: {
            json a = json::array();
            for (const api::Value& item : v.items()) a.push_back(saveValueToJson(rt, item, depth + 1));
            return a;
        }
        case api::Value::Type::Object: {
            json o = json::object();
            for (const auto& [key, item] : v.fields()) {
                if (item.isFunction()) continue;
                o[key] = saveValueToJson(rt, item, depth + 1);
            }
            return o;
        }
        default: return nullptr;
    }
}

api::Value saveValueFromJson(const Runtime& rt, const json& j, int depth = 0) {
    if (depth > 32 || j.is_null()) return {};
    if (j.is_object()) {
        if (const auto v = j.find("$v"); v != j.end() && v->is_array() && v->size() == 3 && j.size() == 1) {
            return api::Value(core::Vec3{(*v)[0].get<float>(), (*v)[1].get<float>(), (*v)[2].get<float>()});
        }
        if (const auto q = j.find("$q"); q != j.end() && q->is_array() && q->size() == 4 && j.size() == 1) {
            return api::Value(core::Quat{(*q)[0].get<float>(), (*q)[1].get<float>(), (*q)[2].get<float>(), (*q)[3].get<float>()});
        }
        if (const auto e = j.find("$ent"); e != j.end() && e->is_string() && j.size() == 1) {
            if (rt.world == nullptr) return {};
            const ecs::Entity found = rt.world->find(Uuid::parse(e->get<std::string>()));
            return found.valid() ? rt.entityValue(found) : api::Value{};
        }
        api::Value o = api::Value::object();
        for (const auto& [key, item] : j.items()) o.set(key, saveValueFromJson(rt, item, depth + 1));
        return o;
    }
    if (j.is_array()) {
        api::Value::Array list;
        list.reserve(j.size());
        for (const json& item : j) list.push_back(saveValueFromJson(rt, item, depth + 1));
        return api::Value(std::move(list));
    }
    if (j.is_boolean()) return api::Value(j.get<bool>());
    if (j.is_number()) return api::Value(j.get<double>());
    if (j.is_string()) return api::Value(j.get<std::string>());
    return {};
}

// "Dialogos/Mercader", "Mercader" o "Mercader.crdialog" -> archivo en Assets.
std::filesystem::path findDialogue(const Runtime& rt, const std::string& name) {
    namespace fs = std::filesystem;
    if (name.empty() || rt.root.empty()) return {};
    fs::path direct = rt.root / pathFromUtf8(name);
    if (direct.extension() != gameplay::kDialogueExtension) direct += gameplay::kDialogueExtension;
    std::error_code e;
    if (fs::is_regular_file(direct, e)) return direct;
    const std::string wanted = lowerText(pathFromUtf8(name).stem().string());
    for (fs::recursive_directory_iterator it(rt.root, fs::directory_options::skip_permission_denied, e);
         !e && it != fs::recursive_directory_iterator(); it.increment(e)) {
        if (it->path().extension() != gameplay::kDialogueExtension) continue;
        if (lowerText(it->path().stem().string()) == wanted) return it->path();
    }
    return {};
}

// Llama a cada callback de la lista (copia: uno puede registrar otro).
void invokeAll(Runtime& rt, const std::vector<api::Value>& list, const api::Value::Array& args) {
    const std::vector<api::Value> copy = list;
    for (const api::Value& fn : copy) rt.native.invoke(fn, args);
}

void addCallback(std::vector<api::Value>& list, const api::Call& c) {
    const api::Value& fn = c.function(0);
    if (fn.isFunction()) list.push_back(fn);
}

api::Value dialogueLineValue(const gameplay::DialogueLine& line) {
    api::Value t = api::Value::object();
    t.set("speaker", line.speaker);
    t.set("text", line.text);
    t.set("audio", line.audio);
    t.set("node", line.node);
    t.set("autoAdvance", line.auto_advance);
    return t;
}

api::Value dialogueChoicesValue(const std::vector<gameplay::DialogueChoice>& choices) {
    api::Value::Array list;
    for (const gameplay::DialogueChoice& choice : choices) {
        api::Value item = api::Value::object();
        item.set("index", choice.index + 1);  // como en Dialogue.choose (desde 1)
        item.set("text", choice.text);
        item.set("enabled", choice.enabled);
        list.push_back(std::move(item));
    }
    return api::Value(std::move(list));
}

api::Value saveInfoValue(const gameplay::SaveSlotInfo& i) {
    api::Value t = api::Value::object();
    t.set("slot", i.slot);
    t.set("label", i.label);
    t.set("scene", i.scene);
    t.set("date", i.date);
    t.set("time", static_cast<double>(i.unix_time));
    t.set("playtime", i.playtime);
    t.set("size", static_cast<double>(i.size_bytes));
    return t;
}

// Argumentos de Text.get / plural / format desde `from`: los objetos son
// argumentos con nombre ({jugador = "Ana"}); lo demas, {0}, {1}...
void collectTextArgs(const api::Call& c, std::size_t from, std::vector<std::string>& args,
                     std::map<std::string, std::string>& named) {
    for (std::size_t i = from; i < c.count(); ++i) {
        const api::Value& a = c.arg(i);
        if (a.isObject()) {
            for (const auto& [key, value] : a.fields()) named[key] = value.asString();
            continue;
        }
        if (a.isArray()) continue;  // una lista no tiene claves de texto
        args.push_back(a.asString());
    }
}

// Un mensaje al script de C++ del objeto (Script::onMessage).
void sendMessage(Runtime& rt, ecs::Entity target, const std::string& name, const api::Value& value) {
    if (rt.message_listener && target.valid()) rt.message_listener(target, name, rt.native.toJson(value).dump());
}

// --- Ciclo del juego ---

void applyPendingSave(Runtime& rt, GameplayState& s) {
    if (!s.save_apply || rt.world == nullptr) return;
    json snapshot = std::move(*s.save_apply);
    s.save_apply.reset();
    std::string error;
    if (!rt.saves.apply(*rt.world, snapshot, &error)) {
        rt.write(2, "Save.load: " + error);
        return;
    }
    rt.flushDestroys();
    s.save_notify_loaded = true;
}

void gameplayStart(Runtime& rt, GameplayState& s) {
    gameplay::setActiveDialogue(&rt.dialogue);
    rt.dialogue.setResolver([&rt](const std::string& name) { return findDialogue(rt, name); });
    rt.saves.setPhysics(rt.physics);
    // El estado de los scripts de C++ vive en su proceso: se les avisa
    // (OnSave / OnLoad) y la partida apunta que el objeto tenia script.
    rt.saves.setScriptHooks(gameplay::SaveSystem::ScriptHooks{
        [&rt](ecs::Entity e) -> json {
            if (!e.has<CppScript>()) return nullptr;
            sendMessage(rt, e, "OnSave", api::Value{});
            return json{{"cpp", true}};
        },
        [&rt](ecs::Entity e, const json& state) {
            if (!state.is_object() || !e.has<CppScript>()) return;
            // Lo que devolvio OnSave() en las partidas de antes (scripts de Lua).
            api::Value data;
            if (const auto it = state.find("data"); it != state.end()) data = saveValueFromJson(rt, *it);
            sendMessage(rt, e, "OnLoad", data);
        }});
    rt.saves.destroy = [&rt](ecs::Entity e) { rt.destroyLater(e.handle()); };
    rt.saves.extra_capture = [&rt]() { return json{{"dialogue", rt.dialogue.variables()}}; };
    rt.saves.extra_restore = [&rt](const json& extra) {
        if (extra.is_object() && extra.contains("dialogue") && extra["dialogue"].is_object()) {
            rt.dialogue.variables() = extra["dialogue"];
        }
    };
    if (rt.world == nullptr) return;
    rt.saves.beginScene(*rt.world, rt.scene_name, "");
    // Partida de otra escena (Save.load): ahora que la escena esta cargada.
    if (rt.saves.pending_load) {
        s.save_apply = std::move(*rt.saves.pending_load);
        rt.saves.pending_load.reset();
        applyPendingSave(rt, s);
    }
}

void gameplayUpdate(Runtime& rt, GameplayState& s, float dt) {
    if (rt.world == nullptr) return;
    applyPendingSave(rt, s);
    if (s.save_notify_loaded) {
        s.save_notify_loaded = false;
        invokeAll(rt, s.save_on_loaded, {api::Value(s.save_loaded_slot)});
    }
    rt.saves.update(*rt.world, dt);
    if (const std::string error = rt.saves.takeWriteError(); !error.empty()) rt.write(2, "Save: " + error);

    rt.dialogue.update(dt);
    for (const gameplay::DialogueEvent& ev : rt.dialogue.takeEvents()) {
        switch (ev.kind) {
            case gameplay::DialogueEvent::Kind::Started:
                invokeAll(rt, s.dialogue_on_start, {api::Value(ev.dialogue)});
                break;
            case gameplay::DialogueEvent::Kind::Line:
                if (s.dialogue_auto_audio && !ev.line.audio.empty() && rt.audio != nullptr) {
                    rt.audio->playOneShot(ev.line.audio, core::Vec3{}, 1.0f, false);
                }
                invokeAll(rt, s.dialogue_on_line, {dialogueLineValue(ev.line)});
                break;
            case gameplay::DialogueEvent::Kind::Choices:
                invokeAll(rt, s.dialogue_on_choices, {dialogueChoicesValue(ev.choices)});
                break;
            case gameplay::DialogueEvent::Kind::Event: {
                invokeAll(rt, s.dialogue_on_event, {api::Value(ev.name), api::Value(ev.argument)});
                // Metodo del script de un objeto de la escena: un mensaje.
                if (!ev.method.empty()) {
                    const ecs::Entity target = ev.target.empty() ? ecs::Entity{} : rt.world->findByName(ev.target);
                    if (!target.valid()) {
                        rt.write(1, "Dialogo: no hay un objeto \"" + ev.target + "\" para el evento " + ev.name);
                    } else {
                        sendMessage(rt, target, ev.method, api::Value(ev.argument));
                    }
                }
                break;
            }
            case gameplay::DialogueEvent::Kind::Ended:
                invokeAll(rt, s.dialogue_on_end, {api::Value(ev.dialogue)});
                break;
        }
    }
}

void gameplayStop(Runtime& rt, GameplayState& s, bool was_running) {
    // Los callbacks y los oyentes del idioma (que apuntan a este sistema) siempre.
    for (const int id : s.language_listeners) gameplay::localization().removeListener(id);
    s.language_listeners.clear();
    s.dialogue_on_start.clear();
    s.dialogue_on_line.clear();
    s.dialogue_on_choices.clear();
    s.dialogue_on_event.clear();
    s.dialogue_on_end.clear();
    s.save_on_loaded.clear();
    if (!was_running) return;
    rt.dialogue.stop();
    rt.dialogue.takeEvents();  // nadie los va a oir
    if (gameplay::activeDialogue() == &rt.dialogue) gameplay::setActiveDialogue(nullptr);
    s.save_apply.reset();  // una carga de la misma escena a medias se pierde al parar
    // Las partidas pendientes de escribir, al disco ya (el editor puede salir de Play y cerrar).
    rt.saves.flush();
    rt.saves.setScriptHooks({});
    rt.saves.destroy = nullptr;
}

// --- Save ---

void registerSave(Runtime& rt, const std::shared_ptr<GameplayState>& state) {
    rt.native.function(
        "Save.save",
        [&rt](api::Call& c) -> api::Value {
            if (rt.world == nullptr) return false;
            std::string error;
            if (!rt.saves.save(*rt.world, c.string(0), c.string(1, ""), &error)) {
                rt.write(2, "Save.save: " + error);
                return false;
            }
            return true;
        },
        {"\"slot1\", \"Etiqueta\"", "guarda la partida (objetos Saveable, valores, dialogos)", "booleano"});
    rt.native.function(
        "Save.load",
        [&rt, state](api::Call& c) -> api::Value {
            const std::string slot = c.string(0);
            json snapshot;
            std::string error;
            if (!rt.saves.read(slot, snapshot, &error)) {
                rt.write(2, "Save.load(\"" + slot + "\"): " + error);
                return false;
            }
            const std::string scene = snapshot.value("scene", std::string());
            if (!scene.empty() && lowerText(scene) != lowerText(rt.scene_name)) {
                // Otra escena: se carga y la partida se aplica al empezar.
                const std::filesystem::path path = rt.findScene(scene);
                if (path.empty()) {
                    rt.write(2, "Save.load: no existe la escena \"" + scene + "\" de la partida");
                    return false;
                }
                rt.saves.pending_load = std::move(snapshot);
                rt.scene_request = path;
                state->save_loaded_slot = slot;
                return true;
            }
            // La misma escena: al principio del siguiente frame (no a mitad de los Update).
            state->save_apply = std::move(snapshot);
            state->save_loaded_slot = slot;
            return true;
        },
        {"\"slot1\"", "carga una partida (cambia de escena si hace falta)", "booleano"});
    rt.native.function("Save.remove", [&rt](api::Call& c) -> api::Value { return rt.saves.remove(c.string(0)); },
                       {"\"slot1\"", "borra una ranura (lo mismo que delete)", "booleano"});
    rt.native.function("Save.delete", [&rt](api::Call& c) -> api::Value { return rt.saves.remove(c.string(0)); },
                       {"\"slot1\"", "borra una ranura", "booleano"});
    rt.native.function("Save.exists", [&rt](api::Call& c) -> api::Value { return rt.saves.exists(c.string(0)); },
                       {"\"slot1\"", "existe?", "booleano"});
    rt.native.function(
        "Save.list",
        [&rt](api::Call&) -> api::Value {
            api::Value::Array list;
            for (const gameplay::SaveSlotInfo& i : rt.saves.list()) list.push_back(saveInfoValue(i));
            return api::Value(std::move(list));
        },
        {"", "{slot, label, scene, date, playtime, size} de cada ranura", "lista de objetos"});
    rt.native.function(
        "Save.info",
        [&rt](api::Call& c) -> api::Value {
            const auto i = rt.saves.info(c.string(0));
            return i ? saveInfoValue(*i) : api::Value{};
        },
        {"\"slot1\"", "datos de una ranura (o nil)", "objeto o nil"});
    rt.native.function(
        "Save.setValue",
        [&rt](api::Call& c) -> api::Value {
            const std::string key = c.string(0);
            if (c.arg(1).isNil()) {
                rt.saves.values().erase(key);
            } else {
                rt.saves.values()[key] = saveValueToJson(rt, c.arg(1));
            }
            return {};
        },
        {"\"oro\", 120", "valor suelto (va en cada partida)"});
    rt.native.function(
        "Save.getValue",
        [&rt](api::Call& c) -> api::Value {
            const auto it = rt.saves.values().find(c.string(0));
            if (it == rt.saves.values().end()) return c.arg(1);
            return saveValueFromJson(rt, *it);
        },
        {"\"oro\", 0", "lee un valor suelto", "valor"});
    rt.native.function("Save.hasValue", [&rt](api::Call& c) -> api::Value { return rt.saves.values().contains(c.string(0)); },
                       {"\"oro\"", "existe?", "booleano"});
    rt.native.function(
        "Save.deleteValue",
        [&rt](api::Call& c) -> api::Value {
            rt.saves.values().erase(c.string(0));
            return {};
        },
        {"\"oro\"", "lo borra"});
    rt.native.function(
        "Save.clearValues",
        [&rt](api::Call&) -> api::Value {
            rt.saves.values() = json::object();
            return {};
        },
        {"", "borra todos"});
    rt.native.function(
        "Save.setAutosave",
        [&rt](api::Call& c) -> api::Value {
            rt.saves.setAutosave(static_cast<float>(c.number(0)), c.string(1, "autosave"));
            return {};
        },
        {"60, \"autosave\"", "autoguardado cada N segundos (0 = no)"});
    rt.native.function("Save.playtime", [&rt](api::Call&) -> api::Value { return rt.saves.playtime(); },
                       {"", "segundos jugados", "numero"});
    rt.native.function(
        "Save.setCompression",
        [&rt](api::Call& c) -> api::Value {
            rt.saves.setCompression(c.boolean(0));
            return {};
        },
        {"true", "partidas comprimidas"});
    rt.native.function("Save.isWriting", [&rt](api::Call&) -> api::Value { return rt.saves.writing(); },
                       {"", "esta escribiendo en segundo plano?", "booleano"});
    rt.native.function(
        "Save.folder",
        [&rt](api::Call&) -> api::Value {
            const std::u8string text = rt.saves.folder().u8string();
            return std::string(text.begin(), text.end());
        },
        {"", "carpeta de las partidas", "texto"});
    rt.native.function(
        "Save.onLoaded",
        [state](api::Call& c) -> api::Value {
            addCallback(state->save_on_loaded, c);
            return {};
        },
        {"function(slot) end", "despues de cargar una partida"});
}

// --- Text (localizacion) ---

void registerText(Runtime& rt, const std::shared_ptr<GameplayState>& state) {
    rt.native.function(
        "Text.get",
        [](api::Call& c) -> api::Value {
            std::vector<std::string> args;
            std::map<std::string, std::string> named;
            collectTextArgs(c, 1, args, named);
            return gameplay::localization().get(c.string(0), args, named);
        },
        {"\"menu.jugar\", ...", "texto en el idioma actual ({0}, {1}... con los argumentos)", "texto"});
    rt.native.function(
        "Text.plural",
        [](api::Call& c) -> api::Value {
            std::vector<std::string> args;
            std::map<std::string, std::string> named;
            collectTextArgs(c, 2, args, named);
            return gameplay::localization().plural(c.string(0), c.number(1), args, named);
        },
        {"\"monedas\", n, ...", "forma plural (clave#one / clave#other) con {n}", "texto"});
    rt.native.function(
        "Text.format",
        [](api::Call& c) -> api::Value {
            std::vector<std::string> args;
            std::map<std::string, std::string> named;
            collectTextArgs(c, 1, args, named);
            return gameplay::Localization::format(c.string(0), args, named);
        },
        {"\"Hola {0}\", nombre", "sustituye {0}, {1}, {nombre}", "texto"});
    rt.native.function("Text.has", [](api::Call& c) -> api::Value { return gameplay::localization().has(c.string(0)); },
                       {"\"clave\"", "existe la clave?", "booleano"});
    rt.native.function("Text.language", [](api::Call&) -> api::Value { return gameplay::localization().language(); },
                       {"", "idioma actual (\"es\")", "texto"});
    rt.native.function(
        "Text.setLanguage",
        [&rt](api::Call& c) -> api::Value {
            const std::string code = c.string(0);
            if (!gameplay::localization().setLanguage(code)) {
                rt.write(1, "Text.setLanguage: el proyecto no tiene el idioma \"" + code + "\"");
                return false;
            }
            return true;
        },
        {"\"en\"", "cambia el idioma (la UI se actualiza sola)", "booleano"});
    rt.native.function(
        "Text.languages",
        [](api::Call&) -> api::Value {
            api::Value::Array list;
            for (const gameplay::LocalizationLanguage& l : gameplay::localization().languages) {
                api::Value item = api::Value::object();
                item.set("code", l.code);
                item.set("name", l.name);
                list.push_back(std::move(item));
            }
            return api::Value(std::move(list));
        },
        {"", "{code, name} de cada idioma", "lista de objetos"});
    rt.native.function("Text.systemLanguage", [](api::Call&) -> api::Value { return gameplay::Localization::systemLanguage(); },
                       {"", "idioma del sistema", "texto"});
    rt.native.function(
        "Text.onLanguageChanged",
        [&rt, state](api::Call& c) -> api::Value {
            const api::Value fn = c.function(0);
            if (!fn.isFunction()) return {};
            const int id = gameplay::localization().addListener(
                [&rt, fn](const std::string& language) { rt.native.invoke(fn, {api::Value(language)}); });
            state->language_listeners.push_back(id);
            return id;
        },
        {"function(codigo) end", "aviso al cambiar de idioma (devuelve un id)", "numero"});
    rt.native.function(
        "Text.removeListener",
        [state](api::Call& c) -> api::Value {
            const int id = static_cast<int>(c.integer(0));
            gameplay::localization().removeListener(id);
            std::vector<int>& ids = state->language_listeners;
            ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
            return {};
        },
        {"id", "quita un aviso"});
}

// --- Dialogue ---

void registerDialogue(Runtime& rt, const std::shared_ptr<GameplayState>& state) {
    rt.native.function(
        "Dialogue.start",
        [&rt](api::Call& c) -> api::Value {
            const std::string name = c.string(0);
            std::string error;
            if (!rt.dialogue.start(name, &error)) {
                rt.write(2, "Dialogue.start(\"" + name + "\"): " + (error.empty() ? std::string("no existe el dialogo") : error));
                return false;
            }
            return true;
        },
        {"\"Mercader\"", "empieza un .crdialog (nombre o ruta)", "booleano"});
    rt.native.function(
        "Dialogue.stop",
        [&rt](api::Call&) -> api::Value {
            rt.dialogue.stop();
            return {};
        },
        {"", "lo corta"});
    rt.native.function(
        "Dialogue.next",
        [&rt](api::Call&) -> api::Value {
            rt.dialogue.advance();
            return {};
        },
        {"", "sigue tras una linea"});
    rt.native.function(
        "Dialogue.advance",
        [&rt](api::Call&) -> api::Value {
            rt.dialogue.advance();
            return {};
        },
        {"", "lo mismo que next"});
    rt.native.function(
        "Dialogue.choose",
        [&rt](api::Call& c) -> api::Value { return rt.dialogue.choose(static_cast<int>(c.integer(0)) - 1); },
        {"1", "elige la opcion 1..n", "booleano"});
    rt.native.function("Dialogue.isActive", [&rt](api::Call&) -> api::Value { return rt.dialogue.active(); },
                       {"", "hay un dialogo en marcha?", "booleano"});
    rt.native.function("Dialogue.isWaitingChoice", [&rt](api::Call&) -> api::Value { return rt.dialogue.waitingChoice(); },
                       {"", "esta esperando que se elija?", "booleano"});
    rt.native.function(
        "Dialogue.name",
        [&rt](api::Call&) -> api::Value { return rt.dialogue.active() ? rt.dialogue.dialogueName() : std::string(); },
        {"", "dialogo que corre", "texto"});
    rt.native.function(
        "Dialogue.currentLine",
        [&rt](api::Call&) -> api::Value {
            if (!rt.dialogue.hasLine()) return {};
            return dialogueLineValue(rt.dialogue.line());
        },
        {"", "{speaker, text, audio, node, autoAdvance} o nil", "objeto o nil"});
    rt.native.function("Dialogue.choices", [&rt](api::Call&) -> api::Value { return dialogueChoicesValue(rt.dialogue.choices()); },
                       {"", "lista de {index, text, enabled}", "lista de objetos"});
    rt.native.function(
        "Dialogue.setVar",
        [&rt](api::Call& c) -> api::Value {
            rt.dialogue.setVariable(c.string(0), saveValueToJson(rt, c.arg(1)));
            return {};
        },
        {"\"oro\", 10", "variable de los dialogos"});
    rt.native.function(
        "Dialogue.getVar",
        [&rt](api::Call& c) -> api::Value { return saveValueFromJson(rt, rt.dialogue.variable(c.string(0))); },
        {"\"oro\"", "lee una variable", "valor"});
    rt.native.function(
        "Dialogue.setAutoAudio",
        [state](api::Call& c) -> api::Value {
            state->dialogue_auto_audio = c.boolean(0);
            return {};
        },
        {"true", "reproduce solo el audio de cada linea"});
    const auto add = [&rt, &state](const char* name, std::vector<api::Value> GameplayState::*list, api::Doc doc) {
        rt.native.function(
            name,
            [state, list](api::Call& c) -> api::Value {
                addCallback((*state).*list, c);
                return {};
            },
            std::move(doc));
    };
    add("Dialogue.onStart", &GameplayState::dialogue_on_start, {"function(nombre) end", "al empezar"});
    add("Dialogue.onLine", &GameplayState::dialogue_on_line, {"function(linea) end", "cada linea"});
    add("Dialogue.onChoices", &GameplayState::dialogue_on_choices, {"function(opciones) end", "opciones para elegir"});
    add("Dialogue.onEvent", &GameplayState::dialogue_on_event, {"function(nombre, argumento) end", "nodo Evento"});
    add("Dialogue.onEnd", &GameplayState::dialogue_on_end, {"function(nombre) end", "al terminar"});
}

}  // namespace

void registerGameplayApi(Runtime& rt) {
    const auto state = std::make_shared<GameplayState>();
    registerSave(rt, state);
    registerText(rt, state);
    registerDialogue(rt, state);
    // Partida pendiente, autoguardado y eventos de dialogo (fase Gameplay).
    rt.onStart([&rt, state] { gameplayStart(rt, *state); });
    rt.onFrame(Phase::Gameplay, [&rt, state](float dt) { gameplayUpdate(rt, *state, dt); });
    rt.onStop([&rt, state](bool was_running) { gameplayStop(rt, *state, was_running); });
}

}  // namespace cramion::scripting::native
