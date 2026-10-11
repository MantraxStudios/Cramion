// Cramion C++ scripting: lo que el motor compila junto a los scripts del
// juego en la DLL (no hace falta tocarlo). Exporta la tabla de clases, sus
// propiedades y los eventos; ninguna excepcion de C++ sale de aqui (se
// devuelven como texto).

#include "Script.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#define CRAMION_EXPORT extern "C" __declspec(dllexport)

namespace cramion {

struct ScriptAccess {
    static void setup(Script& s, std::uint64_t entity, const Value& values) {
        s.entity_ = Entity(entity);
        for (const auto& [name, v] : values.fields()) s.values_[name] = v;
        for (detail::PropertyBase* p : s.properties_) {
            const auto it = s.values_.find(p->name());
            if (it != s.values_.end()) p->load(it->second);
        }
    }
    static const std::vector<detail::PropertyBase*>& properties(const Script& s) { return s.properties_; }
};

namespace detail {

const cppproto::Api* g_api = nullptr;
cppproto::EventArgs g_event{};

namespace {
struct ClassEntry {
    std::string name;
    std::string file;
    Factory factory;
};
std::vector<ClassEntry>& classes() {
    static std::vector<ClassEntry> list;
    return list;
}
void copyError(char* out, int size, const std::string& text) {
    if (out == nullptr || size <= 0) return;
    const std::size_t n = text.size() < static_cast<std::size_t>(size - 1) ? text.size() : static_cast<std::size_t>(size - 1);
    std::memcpy(out, text.data(), n);
    out[n] = 0;
}

// Llamadas sin respuesta en espera.
cppproto::Writer g_batch;
std::uint32_t g_batch_count = 0;

// Callbacks dados a la API (UI, Network.on, Input.bindAction...).
std::uint64_t g_session = 0;
std::uint32_t g_next_callback = 1;
std::unordered_map<std::uint64_t, std::shared_ptr<Callback>> g_callbacks;
std::unordered_map<const Callback*, std::uint64_t> g_callback_ids;
}  // namespace

void registerClass(const char* name, const char* file, Factory factory) { classes().push_back({name, file != nullptr ? file : "", factory}); }

std::vector<CVarDecl>& pendingCVars() {
    static std::vector<CVarDecl> list;
    return list;
}

void registerCVar(const CVarDecl& d) {
    cppproto::Writer w;
    w.str(d.name).u32(d.type).str(d.default_value).str(d.description).u32(d.flags);
    rpc(cppproto::Rpc::CVarRegister, w);
}

void queue(cppproto::Rpc op, const cppproto::Writer& w) {
    g_batch.u32(static_cast<std::uint32_t>(op)).u32(static_cast<std::uint32_t>(w.bytes.size())).raw(w.bytes.data(), w.bytes.size());
    ++g_batch_count;
    if (g_batch.bytes.size() > (1u << 20)) flush();  // no pasarse del tamano de un mensaje
}

void flush() {
    if (g_batch_count == 0 || g_api == nullptr) return;
    cppproto::Writer w;
    w.u32(g_batch_count).raw(g_batch.bytes.data(), g_batch.bytes.size());
    g_batch.bytes.clear();
    g_batch_count = 0;
    const void* reply = nullptr;
    g_api->rpc(static_cast<std::uint32_t>(cppproto::Rpc::Batch), w.bytes.data(), static_cast<std::uint32_t>(w.bytes.size()), &reply);
}

std::uint64_t registerCallback(const std::shared_ptr<Callback>& fn) {
    if (!fn) return 0;
    const auto known = g_callback_ids.find(fn.get());
    if (known != g_callback_ids.end()) return known->second;
    const std::uint64_t id = (g_session << 32) | g_next_callback++;
    g_callbacks[id] = fn;
    g_callback_ids[fn.get()] = id;
    return id;
}

Value luaRequest(const Value& request) {
    cppproto::Writer w;
    w.str(request.toJson());
    auto r = rpc(cppproto::Rpc::Api, w);
    const Value reply = Value::parse(r.str());
    if (!reply["ok"].asBool()) {
        const std::string fn = request["fn"].asString();
        Debug::error("Lua " + (fn.empty() ? request["key"].asString() : fn) + ": " + reply["error"].asString());
        return {};
    }
    return reply["result"];
}

}  // namespace detail

namespace Lua {
void send(std::string_view fn, Values args) {
    cppproto::Writer w;
    w.str(Value{{"op", "call"}, {"fn", fn}, {"args", Value(std::move(args))}}.toJson());
    detail::queue(cppproto::Rpc::ApiSend, w);
}
}  // namespace Lua

}  // namespace cramion

using namespace cramion;

namespace {

std::string describeProperty(const detail::PropInfo& p) {
    Value options = Value::array();
    for (const std::string& o : p.options) options.push(o);
    Value v{{"name", p.name}, {"label", p.label}, {"tooltip", p.tooltip}, {"header", p.header}, {"kind", p.kind},
            {"element", p.element}, {"type", p.type}, {"has_range", p.has_range}, {"min", p.min}, {"max", p.max},
            {"options", options}, {"default", p.default_value}};
    return v.toJson();
}

}  // namespace

CRAMION_EXPORT std::uint32_t cramion_abi() { return cppproto::kAbiVersion; }

CRAMION_EXPORT void cramion_init(const cppproto::Api* api, std::uint64_t session) {
    detail::g_api = api;
    detail::g_session = session;
    for (const detail::CVarDecl& d : detail::pendingCVars()) detail::registerCVar(d);
    detail::pendingCVars().clear();
}

CRAMION_EXPORT int cramion_class_count() { return static_cast<int>(detail::classes().size()); }

CRAMION_EXPORT const char* cramion_class_name(int i) {
    return i >= 0 && i < static_cast<int>(detail::classes().size()) ? detail::classes()[static_cast<std::size_t>(i)].name.c_str() : "";
}

// Las clases con su archivo y sus propiedades (JSON). Devuelve el tamano que
// hace falta (si `size` no llega, se vuelve a pedir).
CRAMION_EXPORT int cramion_describe(char* out, int size, char* error, int error_size) {
    std::string json = "[";
    bool first_class = true;
    for (const auto& c : detail::classes()) {
        std::string props = "[";
        try {
            std::unique_ptr<Script> s(c.factory());
            bool first = true;
            for (const detail::PropertyBase* p : ScriptAccess::properties(*s)) {
                if (!first) props += ",";
                first = false;
                props += describeProperty(p->info());
            }
        } catch (const std::exception& e) {
            detail::copyError(error, error_size, c.name + ": " + e.what());
        } catch (...) {
            detail::copyError(error, error_size, c.name + ": excepcion en el constructor");
        }
        props += "]";
        if (!first_class) json += ",";
        first_class = false;
        json += Value{{"class", c.name}, {"file", c.file}}.toJson();
        json.pop_back();  // }
        json += ",\"properties\":" + props + "}";
    }
    json += "]";
    if (out != nullptr && static_cast<std::size_t>(size) > json.size()) {
        std::memcpy(out, json.data(), json.size());
        out[json.size()] = 0;
    }
    return static_cast<int>(json.size() + 1);
}

CRAMION_EXPORT void* cramion_create(const char* cls, std::uint64_t entity, const char* values_json, char* error, int error_size) {
    for (const auto& c : detail::classes()) {
        if (c.name != cls) continue;
        try {
            std::unique_ptr<Script> script(c.factory());
            ScriptAccess::setup(*script, entity, Value::parse(values_json != nullptr ? values_json : "{}"));
            detail::flush();
            detail::liveScripts().push_back(script.get());
            return script.release();
        } catch (const std::exception& e) {
            detail::copyError(error, error_size, std::string("excepcion en el constructor: ") + e.what());
        } catch (...) {
            detail::copyError(error, error_size, "excepcion desconocida en el constructor");
        }
        return nullptr;
    }
    return nullptr;
}

CRAMION_EXPORT int cramion_call(void* instance, std::uint32_t event, const cppproto::EventArgs* args, char* error,
                                int error_size) {
    auto* s = static_cast<Script*>(instance);
    if (args != nullptr) detail::g_event = *args;
    const float dt = args != nullptr ? args->delta_time : 0.0f;
    Collision c{};
    if (args != nullptr) {
        c.other = Entity(args->other);
        c.point = {args->point[0], args->point[1], args->point[2]};
        c.normal = {args->normal[0], args->normal[1], args->normal[2]};
        c.relativeVelocity = {args->relative_velocity[0], args->relative_velocity[1], args->relative_velocity[2]};
    }
    int result = 0;
    try {
        switch (static_cast<cppproto::Event>(event)) {
            case cppproto::Event::Awake: s->awake(); break;
            case cppproto::Event::Start: s->start(); break;
            case cppproto::Event::Update: s->update(dt); break;
            case cppproto::Event::FixedUpdate: s->fixedUpdate(dt); break;
            case cppproto::Event::LateUpdate: s->lateUpdate(dt); break;
            case cppproto::Event::Destroy: s->onDestroy(); break;
            case cppproto::Event::CollisionEnter: s->onCollisionEnter(c); break;
            case cppproto::Event::CollisionStay: s->onCollisionStay(c); break;
            case cppproto::Event::CollisionExit: s->onCollisionExit(c); break;
            case cppproto::Event::TriggerEnter: s->onTriggerEnter(c.other); break;
            case cppproto::Event::TriggerStay: s->onTriggerStay(c.other); break;
            case cppproto::Event::TriggerExit: s->onTriggerExit(c.other); break;
            default: break;
        }
    } catch (const std::exception& e) {
        detail::copyError(error, error_size, std::string("excepcion: ") + e.what());
        result = 1;
    } catch (...) {
        detail::copyError(error, error_size, "excepcion desconocida");
        result = 1;
    }
    try {
        detail::flush();  // lo que quedo en el lote
    } catch (...) {
    }
    return result;
}

CRAMION_EXPORT int cramion_callback(std::uint64_t id, const char* args_json, char* error, int error_size) {
    const auto it = detail::g_callbacks.find(id);
    if (it == detail::g_callbacks.end()) return 0;  // de una sesion anterior
    int result = 0;
    try {
        const Value args = Value::parse(args_json != nullptr ? args_json : "[]");
        (*it->second)(args.items());
    } catch (const std::exception& e) {
        detail::copyError(error, error_size, std::string("excepcion en un callback: ") + e.what());
        result = 1;
    } catch (...) {
        detail::copyError(error, error_size, "excepcion desconocida en un callback");
        result = 1;
    }
    try {
        detail::flush();
    } catch (...) {
    }
    return result;
}

CRAMION_EXPORT int cramion_message(void* instance, const char* method, const char* value_json, char* error, int error_size) {
    auto* s = static_cast<Script*>(instance);
    try {
        s->onMessage(method != nullptr ? method : "", Value::parse(value_json != nullptr ? value_json : "null"));
        detail::flush();
        return 0;
    } catch (const std::exception& e) {
        detail::copyError(error, error_size, std::string("excepcion en ") + (method != nullptr ? method : "?") + ": " + e.what());
    } catch (...) {
        detail::copyError(error, error_size, std::string("excepcion desconocida en ") + (method != nullptr ? method : "?"));
    }
    return 1;
}

// Recarga en caliente: las propiedades actuales y lo que devuelva
// onBeforeReload(). Devuelve el tamano que hace falta (como cramion_describe);
// si no llega, el proceso vuelve a pedirlo con la misma instancia y se
// reutiliza el texto (onBeforeReload no se llama dos veces).
CRAMION_EXPORT int cramion_snapshot(void* instance, char* out, int size, char* error, int error_size) {
    static void* last_instance = nullptr;
    static std::string last_json;
    auto* s = static_cast<Script*>(instance);
    if (last_instance != instance) {
        last_json.clear();
        try {
            Value props = Value::object();
            for (const detail::PropertyBase* p : ScriptAccess::properties(*s)) props.set(p->name(), p->current());
            Value state = s->onBeforeReload();
            last_json = Value{{"props", props}, {"state", state}}.toJson();
            detail::flush();
        } catch (const std::exception& e) {
            detail::copyError(error, error_size, std::string("excepcion en onBeforeReload: ") + e.what());
            last_json = "{}";
        } catch (...) {
            detail::copyError(error, error_size, "excepcion desconocida en onBeforeReload");
            last_json = "{}";
        }
        last_instance = instance;
    }
    const int needed = static_cast<int>(last_json.size() + 1);
    if (out != nullptr && size >= needed) {
        std::memcpy(out, last_json.data(), last_json.size());
        out[last_json.size()] = 0;
        last_instance = nullptr;  // entregado
    }
    return needed;
}

CRAMION_EXPORT int cramion_restore(void* instance, const char* snapshot_json, char* error, int error_size) {
    auto* s = static_cast<Script*>(instance);
    try {
        const Value snap = Value::parse(snapshot_json != nullptr ? snapshot_json : "{}");
        const Value& props = snap["props"];
        for (detail::PropertyBase* p : ScriptAccess::properties(*s)) {
            const Value v = props[p->name()];
            if (!v.isNil()) p->load(v);
        }
        s->onAfterReload(snap["state"]);
        detail::flush();
        return 0;
    } catch (const std::exception& e) {
        detail::copyError(error, error_size, std::string("excepcion en onAfterReload: ") + e.what());
    } catch (...) {
        detail::copyError(error, error_size, "excepcion desconocida en onAfterReload");
    }
    return 1;
}

CRAMION_EXPORT void cramion_destroy(void* instance) {
    auto& live = detail::liveScripts();
    live.erase(std::remove(live.begin(), live.end(), static_cast<Script*>(instance)), live.end());
    try {
        delete static_cast<Script*>(instance);
        detail::flush();
    } catch (...) {
    }
}
