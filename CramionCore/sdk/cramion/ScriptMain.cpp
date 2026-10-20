// Cramion C++ scripting: lo que el motor compila junto a los scripts del
// juego en la DLL (no hace falta tocarlo). Exporta la tabla de clases, sus
// propiedades y los eventos; ninguna excepcion de C++ sale de aqui (se
// devuelven como texto).

#include "Script.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Lo que exporta la biblioteca de los scripts (DLL en Windows, .so en Linux).
#if defined(_WIN32)
#define CRAMION_EXPORT extern "C" __declspec(dllexport)
#else
#define CRAMION_EXPORT extern "C" __attribute__((visibility("default")))
#endif

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
    std::uint32_t events = ~0u;  // los eventos que sobrescribe (todos si no se sabe)
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

// Callbacks dados a la API (UI, Network.on, Input.bindAction...). Cada uno
// recuerda el script que lo dio (el que se estaba ejecutando) y se borra al
// destruir ese script: si no, el proceso llamaria a un objeto ya borrado.
struct CallbackEntry {
    std::shared_ptr<Callback> fn;
    Script* owner = nullptr;
};
std::uint64_t g_session = 0;
std::uint32_t g_next_callback = 1;
std::unordered_map<std::uint64_t, CallbackEntry> g_callbacks;
std::unordered_map<const Callback*, std::uint64_t> g_callback_ids;
// El script que se esta ejecutando (y, mientras se construye uno, la lista
// donde van los ids que da su constructor, que aun no tiene puntero).
Script* g_current_script = nullptr;
std::vector<std::uint64_t>* g_unowned = nullptr;

// El hilo de los scripts (el que llama a cramion_init) y las llamadas a la API
// hechas desde otros hilos, para avisar desde el bueno: cada una con su numero
// de veces y como mucho kOffThreadKinds distintas hasta el siguiente aviso.
std::thread::id g_script_thread;
std::mutex g_off_mutex;
std::vector<std::pair<std::string, std::uint32_t>> g_off_calls;
std::uint32_t g_off_more = 0;  // las que no cupieron
std::atomic<bool> g_off_pending{false};
constexpr std::size_t kOffThreadKinds = 16;

bool onScriptThread() { return g_script_thread == std::thread::id() || std::this_thread::get_id() == g_script_thread; }

const char* rpcName(cppproto::Rpc op) {
    using R = cppproto::Rpc;
    switch (op) {
        case R::Log: return "Debug::log";
        case R::FindEntity: return "Scene::find";
        case R::FindWithTag: return "Scene::findWithTag";
        case R::CreateEntity: return "Scene::create";
        case R::Destroy: return "Entity::destroy";
        case R::Valid: return "Entity::valid";
        case R::GetName: case R::SetName: return "Entity (nombre)";
        case R::GetTag: return "Entity::tag";
        case R::GetVec: case R::SetVec: return "Entity (transformacion o velocidad)";
        case R::GetActive: case R::SetActive: return "Entity (activo)";
        case R::Parent: return "Entity::parent";
        case R::HasComponent: case R::AddComponent: case R::GetComponent: case R::SetComponent: return "Entity (componentes)";
        case R::GetField: case R::SetField: return "Entity (campos)";
        case R::Key: case R::MouseButton: case R::Axis: case R::Mouse: return "Input";
        case R::Raycast: return "Physics::raycast";
        case R::CharMove: case R::CharGrounded: case R::CharInput: case R::CharJump: case R::CharCrouch: return "CharacterController";
        case R::CVarGet: case R::CVarSet: case R::CVarRegister: return "CVar";
        case R::AddForce: return "Entity::addForce";
        case R::Api: case R::ApiSend: return "Api";
        case R::Batch: return "lote";
        case R::GetPositions: case R::SetPositions: return "Scene::getPositions/setPositions";
        case R::ApiResolve: case R::ApiCall: case R::ApiCallSend: return "Api";
        case R::SetTransforms: return "Scene::setTransforms / Entity::setQuaternion";
        default: return "API";
    }
}

// Los avisos de las llamadas desde otro hilo (solo desde el hilo de los
// scripts; directo al proceso, sin pasar por el lote).
void reportOffThread() {
    if (!g_off_pending.exchange(false, std::memory_order_acq_rel) || g_api == nullptr) return;
    std::vector<std::pair<std::string, std::uint32_t>> calls;
    std::uint32_t more = 0;
    {
        const std::lock_guard<std::mutex> lock(g_off_mutex);
        calls.swap(g_off_calls);
        more = std::exchange(g_off_more, 0u);
    }
    for (std::size_t i = 0; i < calls.size(); ++i) {
        std::string text = calls[i].first + " llamado desde otro hilo";
        if (calls[i].second > 1) text += " (" + std::to_string(calls[i].second) + " veces)";
        text += ": no se envio y devolvio un valor vacio. La API de Cramion solo funciona en el hilo de los scripts "
                "(awake, update, callbacks...): deja los datos del otro hilo en una cola con std::mutex y usalos desde update()";
        if (i + 1 == calls.size() && more > 0) text += " (y " + std::to_string(more) + " llamadas mas desde otros hilos)";
        cppproto::Writer w;
        w.u32(2).str(text);
        const void* reply = nullptr;
        g_api->rpc(static_cast<std::uint32_t>(cppproto::Rpc::Log), w.bytes.data(), static_cast<std::uint32_t>(w.bytes.size()), &reply);
    }
}

// Marca el script en curso mientras dura (y deja el anterior al salir).
struct CurrentScript {
    Script* prev;
    std::vector<std::uint64_t>* prev_unowned;
    explicit CurrentScript(Script* s, std::vector<std::uint64_t>* unowned = nullptr)
        : prev(g_current_script), prev_unowned(g_unowned) {
        g_current_script = s;
        g_unowned = unowned;
    }
    ~CurrentScript() {
        g_current_script = prev;
        g_unowned = prev_unowned;
    }
    CurrentScript(const CurrentScript&) = delete;
    CurrentScript& operator=(const CurrentScript&) = delete;
};

void eraseCallback(std::uint64_t id) {
    const auto it = g_callbacks.find(id);
    if (it == g_callbacks.end()) return;
    g_callback_ids.erase(it->second.fn.get());
    g_callbacks.erase(it);
}

void eraseCallbacksOf(const Script* owner) {
    if (owner == nullptr) return;
    for (auto it = g_callbacks.begin(); it != g_callbacks.end();) {
        if (it->second.owner == owner) {
            g_callback_ids.erase(it->second.fn.get());
            it = g_callbacks.erase(it);
        } else {
            ++it;
        }
    }
}
}  // namespace

void registerClass(const char* name, const char* file, Factory factory) { classes().push_back({name, file != nullptr ? file : "", factory}); }

void registerClass(const char* name, const char* file, Factory factory, std::uint32_t events) {
    classes().push_back({name, file != nullptr ? file : "", factory, events});
}

std::vector<CVarDecl>& pendingCVars() {
    static std::vector<CVarDecl> list;
    return list;
}

void registerCVar(const CVarDecl& d) {
    cppproto::Writer w;
    w.str(d.name).u32(d.type).str(d.default_value).str(d.description).u32(d.flags);
    rpc(cppproto::Rpc::CVarRegister, w);
}

bool offScriptThread(std::string_view what) {
    if (onScriptThread()) return false;
    {
        const std::lock_guard<std::mutex> lock(g_off_mutex);
        bool known = false;
        for (auto& [name, count] : g_off_calls) {
            if (name == what) {
                ++count;
                known = true;
                break;
            }
        }
        if (!known && g_off_calls.size() < kOffThreadKinds) g_off_calls.emplace_back(std::string(what), 1u);
        else if (!known) ++g_off_more;
    }
    g_off_pending.store(true, std::memory_order_release);
    return true;
}

bool offScriptThread(cppproto::Rpc op) { return !onScriptThread() && offScriptThread(rpcName(op)); }

void queue(cppproto::Rpc op, const cppproto::Writer& w) {
    if (offScriptThread(op)) return;
    g_batch.u32(static_cast<std::uint32_t>(op)).u32(static_cast<std::uint32_t>(w.bytes.size())).raw(w.bytes.data(), w.bytes.size());
    ++g_batch_count;
    if (g_batch.bytes.size() > (1u << 20)) flush();  // no pasarse del tamano de un mensaje
}

void flush() {
    if (!onScriptThread()) return;  // el lote es del hilo de los scripts (offScriptThread ya aviso)
    if (g_off_pending.load(std::memory_order_acquire)) reportOffThread();
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
    // Los 32 bits altos son el proceso sin el bit 63, que es de los callbacks
    // locales del motor (NativeApi::kLocalCallbackBase); el proceso compara igual.
    std::uint32_t n = g_next_callback++;
    if (n == 0) n = g_next_callback++;  // el 0 es "sin callback"
    const std::uint64_t id = ((g_session & 0x7FFFFFFFull) << 32) | n;
    g_callbacks[id] = CallbackEntry{fn, g_current_script};
    g_callback_ids[fn.get()] = id;
    if (g_current_script == nullptr && g_unowned != nullptr) g_unowned->push_back(id);
    return id;
}

// --- La API por indice y en binario (Script.h, apiFast) ---------------------------
namespace {

// Una clave ya resuelta: su id en el motor y si puede ir en lote. kind: 0 de
// una tabla, 1 de una entidad ("Entity:x"), 2 de un handle (":x", el motor
// busca por el tipo de self).
struct ApiSlot {
    std::uint8_t kind = 0;
    std::string name;
    std::uint32_t id = 0;
    bool batch = false;
};
// Por hash de (kind, nombre); cada cubo con sus claves (casi siempre una). Se
// vacia al empezar la sesion y si el motor cambia de epoca.
std::unordered_map<std::uint64_t, std::vector<ApiSlot>> g_api_slots;
std::uint32_t g_api_epoch = 0;
// El mensaje de cada llamada (sin reservar memoria en cada una).
cppproto::Writer g_api_writer;

std::uint64_t slotHash(std::uint8_t kind, std::string_view name) {
    std::uint64_t h = 1469598103934665603ull ^ kind;
    for (const char c : name) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h;
}

const ApiSlot* findSlot(std::uint64_t hash, std::uint8_t kind, std::string_view name) {
    const auto it = g_api_slots.find(hash);
    if (it == g_api_slots.end()) return nullptr;
    for (const ApiSlot& s : it->second) {
        if (s.kind == kind && s.name == name) return &s;
    }
    return nullptr;
}

// Pide el id al motor (una vez por clave y sesion). nullptr: el motor no da
// ids (fuera de Play, el proceso que describe las clases): se usa el JSON.
const ApiSlot* resolveSlot(std::uint64_t hash, std::uint8_t kind, std::string_view name) {
    const bool qualified = name.find(':') != std::string_view::npos;
    std::string key;
    if (kind == 1 && !qualified) key = "Entity:";
    else if (kind == 2 && !qualified) key = ":";
    key.append(name);
    cppproto::Writer w;
    w.str(key);
    auto r = rpc(cppproto::Rpc::ApiResolve, w);
    const std::uint32_t epoch = r.u32();
    const std::uint32_t id = r.u32();
    const std::uint32_t flags = r.u32();
    if (!r.ok() || epoch == 0) return nullptr;
    if (epoch != g_api_epoch) {
        g_api_slots.clear();  // ids de otro ScriptSystem
        g_api_epoch = epoch;
    }
    std::vector<ApiSlot>& bucket = g_api_slots[hash];
    bucket.push_back(ApiSlot{kind, std::string(name), id, (flags & cppproto::ApiBatchable) != 0});
    return &bucket.back();
}

void writeValue(cppproto::Writer& w, const Value& v, int depth = 0) {
    using cppproto::ValueTag;
    const auto tag = [&](ValueTag t) { w.u8(static_cast<std::uint8_t>(t)); };
    if (depth > 16) {
        tag(ValueTag::Nil);
        return;
    }
    switch (v.type()) {
        case Value::Type::Nil: tag(ValueTag::Nil); return;
        case Value::Type::Bool: tag(v.asBool() ? ValueTag::True : ValueTag::False); return;
        case Value::Type::Number: {
            tag(ValueTag::Number);
            w.f64(v.asNumber());
            return;
        }
        case Value::Type::String: {
            tag(ValueTag::String);
            w.str(v.asString());
            return;
        }
        case Value::Type::Vec3: {
            const Vec3 p = v.asVec3();
            tag(ValueTag::Vec3);
            w.f32(p.x).f32(p.y).f32(p.z);
            return;
        }
        case Value::Type::Quat: {
            const Quat q = v.asQuat();
            tag(ValueTag::Quat);
            w.f32(q.x).f32(q.y).f32(q.z).f32(q.w);
            return;
        }
        case Value::Type::Entity: {
            tag(ValueTag::Entity);
            w.u64(v.id());
            return;
        }
        case Value::Type::Handle: {
            tag(ValueTag::Handle);
            w.u64(v.id());
            return;
        }
        case Value::Type::Array: {
            const Values& items = v.items();
            tag(ValueTag::Array);
            w.u32(static_cast<std::uint32_t>(items.size()));
            for (const Value& item : items) writeValue(w, item, depth + 1);
            return;
        }
        case Value::Type::Object: {
            const Value::Object& fields = v.fields();
            tag(ValueTag::Object);
            w.u32(static_cast<std::uint32_t>(fields.size()));
            for (const auto& [key, item] : fields) {
                w.str(key);
                writeValue(w, item, depth + 1);
            }
            return;
        }
        case Value::Type::Function:
            // Un callback: su JSON ({"$f":id}) lo registra (con su script).
            tag(ValueTag::Json);
            w.str(v.toJson());
            return;
    }
    tag(ValueTag::Nil);
}

Value readValue(cppproto::Reader& r, int depth = 0) {
    using cppproto::ValueTag;
    const auto tag = static_cast<ValueTag>(r.u8());
    if (!r.ok()) return {};
    switch (tag) {
        case ValueTag::Nil: return {};
        case ValueTag::False: return Value(false);
        case ValueTag::True: return Value(true);
        case ValueTag::Number: return Value(r.f64());
        case ValueTag::String: return Value(r.str());
        case ValueTag::Vec3: {
            float v[3];
            r.vec3(v);
            return Value(Vec3{v[0], v[1], v[2]});
        }
        case ValueTag::Quat: {
            const float x = r.f32(), y = r.f32(), z = r.f32(), w = r.f32();
            return Value(Quat{x, y, z, w});
        }
        case ValueTag::Entity: return Value::entityId(r.u64());
        case ValueTag::Handle: return Value::handle(r.u64());
        // Un callback del motor no se puede llamar desde aqui: como en el JSON, {"$f": id}.
        case ValueTag::Function: return Value{{"$f", Value(static_cast<double>(r.u64()))}};
        case ValueTag::Array: {
            const std::uint32_t n = r.u32();
            if (!r.ok() || n > r.remaining()) return {};
            Values list;
            list.reserve(n);
            for (std::uint32_t i = 0; i < n && r.ok(); ++i) list.push_back(readValue(r, depth + 1));
            return Value(std::move(list));
        }
        case ValueTag::Object: {
            const std::uint32_t n = r.u32();
            if (!r.ok() || n > r.remaining()) return {};
            Value o = Value::object();
            for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
                const std::string key = r.str();
                o.set(key, readValue(r, depth + 1));
            }
            return o;
        }
        case ValueTag::Json: return Value::parse(r.str());
    }
    return {};
}

}  // namespace

void resetApiSlots() {
    g_api_slots.clear();
    g_api_epoch = 0;
}

namespace {

// El camino de antes (JSON con nombres): sin ids del motor.
Value apiJson(cppproto::ApiOp op, std::string_view key, const Value* self, const Value* args, std::size_t count) {
    Value request = Value::object();
    const char* op_name = op == cppproto::ApiOp::Get ? "get" : (op == cppproto::ApiOp::Set ? "set" : "call");
    request.set("op", op_name);
    if (self != nullptr) request.set("self", *self);
    // Las funciones y los metodos van por "fn"; las propiedades de un objeto, por "key".
    request.set(self != nullptr && op != cppproto::ApiOp::Call ? "key" : "fn", key);
    if (op == cppproto::ApiOp::Set) {
        request.set("value", count > 0 ? args[0] : Value());
    } else if (op == cppproto::ApiOp::Call) {
        request.set("args", Value(Values(args, args + count)));
    }
    return apiRequest(request);
}

}  // namespace

Value apiFast(cppproto::ApiOp op, std::string_view key, const Value* self, const Value* args, std::size_t count) {
    if (!onScriptThread()) {
        offScriptThread(key.empty() ? std::string_view("Api") : key);
        return {};
    }
    const std::uint8_t kind = self == nullptr ? 0 : (self->type() == Value::Type::Entity ? 1 : 2);
    const std::uint64_t hash = slotHash(kind, key);
    // Dos vueltas: si el motor dice que los ids son de otra sesion (Stale), se piden otra vez.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const ApiSlot* slot = findSlot(hash, kind, key);
        if (slot == nullptr) slot = resolveSlot(hash, kind, key);
        if (slot == nullptr) return apiJson(op, key, self, args, count);
        const std::uint32_t id = slot->id;
        const bool batch = op == cppproto::ApiOp::Set || (op == cppproto::ApiOp::Call && slot->batch);
        cppproto::Writer& w = g_api_writer;
        w.bytes.clear();
        w.u32(g_api_epoch).u32(id).u32(static_cast<std::uint32_t>(op));
        writeValue(w, self != nullptr ? *self : Value());
        w.u32(static_cast<std::uint32_t>(count));
        for (std::size_t i = 0; i < count; ++i) writeValue(w, args[i]);
        if (batch) {
            queue(cppproto::Rpc::ApiCallSend, w);
            return {};
        }
        auto r = rpc(cppproto::Rpc::ApiCall, w);
        const auto status = static_cast<cppproto::ApiStatus>(r.u32());
        if (!r.ok()) {
            Debug::error("API " + std::string(key) + ": el motor no respondio");
            return {};
        }
        if (status == cppproto::ApiStatus::Ok) return readValue(r);
        if (status == cppproto::ApiStatus::Stale) {
            g_api_slots.clear();
            g_api_epoch = 0;
            continue;
        }
        Debug::error("API " + std::string(key) + ": " + r.str());
        return {};
    }
    return {};
}

Value apiRequest(const Value& request) {
    if (!onScriptThread()) {
        const std::string fn = request["fn"].asString();
        offScriptThread(fn.empty() ? "Api (" + request["key"].asString() + ")" : fn);
        return {};
    }
    cppproto::Writer w;
    w.str(request.toJson());
    auto r = rpc(cppproto::Rpc::Api, w);
    const Value reply = Value::parse(r.str());
    if (!reply["ok"].asBool()) {
        const std::string fn = request["fn"].asString();
        Debug::error("API " + (fn.empty() ? request["key"].asString() : fn) + ": " + reply["error"].asString());
        return {};
    }
    return reply["result"];
}

}  // namespace detail

namespace Api {
void send(std::string_view fn, Values args) {
    if (detail::offScriptThread(fn)) return;
    cppproto::Writer w;
    w.str(Value{{"op", "call"}, {"fn", fn}, {"args", Value(std::move(args))}}.toJson());
    detail::queue(cppproto::Rpc::ApiSend, w);
}
}  // namespace Api

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
    detail::g_script_thread = std::this_thread::get_id();
    detail::g_api = api;
    detail::g_session = session;
    detail::resetApiSlots();  // los ids de la API son de cada sesion del motor
    for (const detail::CVarDecl& d : detail::pendingCVars()) detail::registerCVar(d);
    detail::pendingCVars().clear();
}

CRAMION_EXPORT int cramion_class_count() { return static_cast<int>(detail::classes().size()); }

CRAMION_EXPORT const char* cramion_class_name(int i) {
    return i >= 0 && i < static_cast<int>(detail::classes().size()) ? detail::classes()[static_cast<std::size_t>(i)].name.c_str() : "";
}

// Los eventos que sobrescribe la clase i (bit 1 << cppproto::Event): el motor
// no envia los demas. Opcional: sin esta exportacion (DLL anterior), todos.
CRAMION_EXPORT std::uint32_t cramion_class_events(int i) {
    return i >= 0 && i < static_cast<int>(detail::classes().size()) ? detail::classes()[static_cast<std::size_t>(i)].events : ~0u;
}

// Las clases con su archivo y sus propiedades (JSON). Devuelve el tamano que
// hace falta (si `size` no llega, se vuelve a pedir).
CRAMION_EXPORT int cramion_describe(char* out, int size, char* error, int error_size) {
    std::string json = "[";
    bool first_class = true;
    for (const auto& c : detail::classes()) {
        std::string props = "[";
        // Una instancia de usar y tirar: lo que registre su constructor se borra.
        std::vector<std::uint64_t> unowned;
        try {
            const detail::CurrentScript scope(nullptr, &unowned);
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
        for (const std::uint64_t id : unowned) detail::eraseCallback(id);
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
        // Lo que registre el constructor (aun sin puntero) pasa al script al
        // terminar; si algo falla, se borra.
        std::vector<std::uint64_t> unowned;
        try {
            std::unique_ptr<Script> script;
            {
                const detail::CurrentScript creating(nullptr, &unowned);
                script.reset(c.factory());
            }
            for (const std::uint64_t id : unowned) {
                const auto it = detail::g_callbacks.find(id);
                if (it != detail::g_callbacks.end()) it->second.owner = script.get();
            }
            unowned.clear();
            const detail::CurrentScript scope(script.get());
            try {
                ScriptAccess::setup(*script, entity, Value::parse(values_json != nullptr ? values_json : "{}"));
                detail::flush();
            } catch (...) {
                detail::eraseCallbacksOf(script.get());
                throw;
            }
            detail::liveScripts().push_back(script.get());
            return script.release();
        } catch (const std::exception& e) {
            detail::copyError(error, error_size, std::string("excepcion en el constructor: ") + e.what());
        } catch (...) {
            detail::copyError(error, error_size, "excepcion desconocida en el constructor");
        }
        for (const std::uint64_t id : unowned) detail::eraseCallback(id);
        return nullptr;
    }
    return nullptr;
}

CRAMION_EXPORT int cramion_call(void* instance, std::uint32_t event, const cppproto::EventArgs* args, char* error,
                                int error_size) {
    auto* s = static_cast<Script*>(instance);
    const detail::CurrentScript scope(s);
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
            case cppproto::Event::Enable: s->onEnable(); break;
            case cppproto::Event::Disable: s->onDisable(); break;
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
    if (it == detail::g_callbacks.end()) return 0;  // de una sesion anterior o de un script ya destruido
    // Copia: el callback puede registrar otros (rehash) o borrarse a si mismo.
    const std::shared_ptr<Callback> fn = it->second.fn;
    const detail::CurrentScript scope(it->second.owner);
    int result = 0;
    try {
        const Value args = Value::parse(args_json != nullptr ? args_json : "[]");
        (*fn)(args.items());
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
    const detail::CurrentScript scope(s);
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
    const detail::CurrentScript scope(s);
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
    const detail::CurrentScript scope(s);
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
    auto* s = static_cast<Script*>(instance);
    live.erase(std::remove(live.begin(), live.end(), s), live.end());
    detail::eraseCallbacksOf(s);
    // Lo que registre el destructor tampoco sobrevive al script.
    std::vector<std::uint64_t> unowned;
    try {
        {
            const detail::CurrentScript scope(nullptr, &unowned);
            delete s;
        }
        detail::flush();
    } catch (...) {
    }
    for (const std::uint64_t id : unowned) detail::eraseCallback(id);
}
