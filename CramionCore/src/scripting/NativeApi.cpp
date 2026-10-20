#include "CramionCore/scripting/NativeApi.h"

#include <CramionFX/core/Chars.h>
#include <cramion/CppProtocol.h>
#include <entt/entity/entity.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace cramion::scripting::api {

namespace {

const std::shared_ptr<Handle> kNoHandle;

std::string formatNumber(double d) {
    char buffer[40];
    if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e15) {
        const auto r = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<long long>(d));
        return std::string(buffer, r.ptr);
    }
    std::snprintf(buffer, sizeof(buffer), "%.14g", d);
    return buffer;
}

// Igual que el SDK y el puente de los scripts: la entidad viaja como su handle + 1 (0 = ninguna).
std::uint64_t entityId(entt::entity e) {
    return e == entt::null ? 0 : static_cast<std::uint64_t>(entt::to_integral(e)) + 1;
}

entt::entity entityFromId(std::uint64_t id) {
    return id == 0 ? entt::entity{entt::null}
                   : static_cast<entt::entity>(static_cast<std::underlying_type_t<entt::entity>>(id - 1));
}

const char* typeName(Value::Type t) {
    switch (t) {
        case Value::Type::Nil: return "nil";
        case Value::Type::Bool: return "bool";
        case Value::Type::Number: return "numero";
        case Value::Type::String: return "texto";
        case Value::Type::Vec3: return "Vec3";
        case Value::Type::Quat: return "Quat";
        case Value::Type::Entity: return "entidad";
        case Value::Type::Array: return "lista";
        case Value::Type::Object: return "objeto";
        case Value::Type::Function: return "funcion";
        case Value::Type::Handle: return "objeto del motor";
    }
    return "?";
}

}  // namespace

// --- Value -----------------------------------------------------------------------

Value Value::entity(entt::entity e) {
    Value v;
    v.type_ = Type::Entity;
    v.entity_ = e;
    return v;
}

Value Value::object() {
    Value v;
    v.type_ = Type::Object;
    v.object_ = std::make_shared<Object>();
    return v;
}

Value Value::function(std::uint64_t callback_id) {
    Value v;
    v.type_ = Type::Function;
    v.id_ = callback_id;
    return v;
}

Value Value::handle(std::shared_ptr<Handle> h) {
    if (!h) return {};
    Value v;
    v.type_ = Type::Handle;
    v.handle_ = std::move(h);
    return v;
}

double Value::asNumber(double fallback) const {
    if (type_ == Type::Number || type_ == Type::Bool) return number_;
    if (type_ == Type::String) {
        double d = fallback;
        core::parseFloat(text_.data(), text_.data() + text_.size(), d);
        return d;
    }
    return fallback;
}

std::string Value::asString() const {
    switch (type_) {
        case Type::Nil: return "nil";
        case Type::Bool: return number_ != 0.0 ? "true" : "false";
        case Type::Number: return formatNumber(number_);
        case Type::String: return text_;
        case Type::Vec3: {
            char buffer[96];
            std::snprintf(buffer, sizeof(buffer), "(%.3f, %.3f, %.3f)", v_[0], v_[1], v_[2]);
            return buffer;
        }
        case Type::Quat: {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "(%.3f, %.3f, %.3f, %.3f)", v_[0], v_[1], v_[2], v_[3]);
            return buffer;
        }
        case Type::Entity: return entity_ == entt::null ? "Entity(nil)" : "Entity(" + std::to_string(entityId(entity_) - 1) + ")";
        case Type::Array: {
            std::string out = "[";
            for (std::size_t i = 0; i < array_->size(); ++i) {
                if (i > 0) out += ", ";
                out += (*array_)[i].asString();
            }
            return out + "]";
        }
        case Type::Object: {
            std::string out = "{";
            bool first = true;
            for (const auto& [k, v] : *object_) {
                if (!first) out += ", ";
                first = false;
                out += k + " = " + v.asString();
            }
            return out + "}";
        }
        case Type::Function: return "function";
        case Type::Handle: return std::string(handle_->typeName());
    }
    return {};
}

const std::shared_ptr<Handle>& Value::asHandle() const { return type_ == Type::Handle ? handle_ : kNoHandle; }

std::size_t Value::size() const {
    if (type_ == Type::Array) return array_->size();
    if (type_ == Type::Object) return object_->size();
    return 0;
}

const Value& Value::operator[](std::size_t i) const {
    return type_ == Type::Array && i < array_->size() ? (*array_)[i] : nil();
}

const Value& Value::operator[](std::string_view key) const {
    if (type_ != Type::Object) return nil();
    for (const auto& [k, v] : *object_) {
        if (k == key) return v;
    }
    return nil();
}

void Value::push(Value v) {
    if (type_ != Type::Array) *this = Value(Array{});
    array_->push_back(std::move(v));
}

void Value::set(std::string_view key, Value v) {
    if (type_ != Type::Object) *this = object();
    for (auto& [k, old] : *object_) {
        if (k == key) {
            old = std::move(v);
            return;
        }
    }
    object_->emplace_back(std::string(key), std::move(v));
}

const Value::Array& Value::items() const {
    static const Array empty;
    return type_ == Type::Array ? *array_ : empty;
}

const Value::Object& Value::fields() const {
    static const Object empty;
    return type_ == Type::Object ? *object_ : empty;
}

const Value& Value::nil() {
    static const Value n;
    return n;
}

// --- Call --------------------------------------------------------------------------

void Call::wrong(std::size_t i, const char* expected) const {
    throw Error("argumento " + std::to_string(i + 1) + ": se esperaba " + expected + " (llego " +
                typeName(arg(i).type()) + ")");
}

double Call::number(std::size_t i) const {
    const Value& v = arg(i);
    if (v.type() != Value::Type::Number) wrong(i, "un numero");
    return v.asNumber();
}

double Call::number(std::size_t i, double fallback) const { return has(i) ? number(i) : fallback; }

bool Call::boolean(std::size_t i, bool fallback) const { return has(i) ? arg(i).truthy() : fallback; }

std::string Call::string(std::size_t i) const {
    const Value& v = arg(i);
    if (v.type() != Value::Type::String && v.type() != Value::Type::Number) wrong(i, "un texto");
    return v.asString();
}

std::string Call::string(std::size_t i, std::string_view fallback) const { return has(i) ? string(i) : std::string(fallback); }

core::Vec3 Call::vec3(std::size_t i) const {
    const Value& v = arg(i);
    if (v.type() != Value::Type::Vec3) wrong(i, "un Vec3");
    return v.asVec3();
}

core::Vec3 Call::vec3(std::size_t i, const core::Vec3& fallback) const { return has(i) ? vec3(i) : fallback; }

long long Call::integer(std::size_t i) const { return std::llround(number(i)); }

long long Call::integer(std::size_t i, long long fallback) const { return has(i) ? integer(i) : fallback; }

core::Quat Call::quat(std::size_t i) const {
    const Value& v = arg(i);
    if (v.type() != Value::Type::Quat) wrong(i, "un Quat");
    return v.asQuat();
}

core::Quat Call::quat(std::size_t i, const core::Quat& fallback) const { return has(i) ? quat(i) : fallback; }

const Value& Call::object(std::size_t i) const {
    const Value& v = arg(i);
    if (!v.isNil() && v.type() != Value::Type::Object) wrong(i, "un objeto {clave = valor}");
    return v;
}

const Value& Call::list(std::size_t i) const {
    const Value& v = arg(i);
    if (!v.isNil() && v.type() != Value::Type::Array) wrong(i, "una lista");
    return v;
}

entt::entity Call::entity(std::size_t i) const {
    const Value& v = arg(i);
    if (v.isNil()) return entt::null;
    if (v.type() != Value::Type::Entity) wrong(i, "una entidad");
    return v.asEntity();
}

entt::entity Call::selfEntity() const {
    if (self_.type() != Value::Type::Entity) throw Error("se esperaba una entidad como self");
    return self_.asEntity();
}

const Value& Call::function(std::size_t i) const {
    const Value& v = arg(i);
    if (!v.isNil() && v.type() != Value::Type::Function) wrong(i, "una funcion");
    return v;
}

// --- NativeApi -----------------------------------------------------------------------

void NativeApi::add(Entry e) {
    // Las funciones globales (print) van solo con su nombre.
    const std::string key = e.owner.empty() ? e.name : e.owner + (e.member() ? ":" : ".") + e.name;
    const auto found = index_.find(key);
    if (found != index_.end()) {
        entries_[found->second] = std::move(e);  // registrar otra vez la sustituye
        return;
    }
    index_.emplace(key, entries_.size());
    entries_.push_back(std::move(e));
}

void NativeApi::function(std::string path, Function fn, Doc doc) {
    const std::size_t dot = path.rfind('.');
    Entry e;
    e.kind = Entry::Kind::Function;
    e.owner = dot == std::string::npos ? std::string{} : path.substr(0, dot);
    e.name = dot == std::string::npos ? path : path.substr(dot + 1);
    e.call = std::move(fn);
    e.doc = std::move(doc);
    add(std::move(e));
}

void NativeApi::method(std::string type, std::string name, Function fn, Doc doc) {
    Entry e;
    e.kind = Entry::Kind::Method;
    e.owner = std::move(type);
    e.name = std::move(name);
    e.call = std::move(fn);
    e.doc = std::move(doc);
    add(std::move(e));
}

void NativeApi::property(std::string owner, std::string name, Function get, Function set, Doc doc, bool type_member) {
    Entry e;
    e.kind = Entry::Kind::Property;
    e.owner = std::move(owner);
    e.name = std::move(name);
    e.call = std::move(get);
    e.assign = std::move(set);
    e.doc = std::move(doc);
    e.type_member = type_member;
    add(std::move(e));
}

const Entry* NativeApi::find(std::string_view key) const {
    const auto found = index_.find(std::string(key));
    return found == index_.end() ? nullptr : &entries_[found->second];
}

void NativeApi::dynamicProperties(std::string owner, Function get, Function set) {
    dynamic_[std::move(owner)] = Dynamic{std::move(get), std::move(set)};
}

// El duenio dinamico mas largo que es prefijo del nombre: "Graphics.post.x[1].y"
// es de "Graphics.post" con la clave "x[1].y" (no de "Graphics").
const NativeApi::Dynamic* NativeApi::dynamicFor(const std::string& full, std::string& key) const {
    const Dynamic* best = nullptr;
    std::size_t best_length = 0;
    for (const auto& [owner, dynamic] : dynamic_) {
        if (owner.size() >= full.size() || owner.size() <= best_length) continue;
        if (full.compare(0, owner.size(), owner) != 0) continue;
        const char separator = full[owner.size()];
        if (separator != '.' && separator != ':') continue;
        best = &dynamic;
        best_length = owner.size();
    }
    if (best != nullptr) key = full.substr(best_length + 1);
    return best;
}

// El duenio de un metodo o propiedad de un objeto: "Entity" o el tipo del handle.
namespace {
std::string ownerOf(const Value& self) {
    if (self.type() == Value::Type::Entity) return "Entity";
    if (self.type() == Value::Type::Handle) return std::string(self.asHandle()->typeName());
    return {};
}
}  // namespace

const Entry* NativeApi::resolve(std::string_view key, const Value& self, std::string& full) const {
    full = std::string(key);
    if (!self.isNil() && full.find(':') == std::string::npos) {
        const std::string owner = ownerOf(self);
        if (owner.empty()) throw Error("el objeto ya no existe");
        full = owner + ":" + full;
    }
    return find(full);
}

Value NativeApi::call(std::string_view key, const Value::Array& args, const Value& self) {
    std::string full;
    const Entry* entry = resolve(key, self, full);
    if (entry == nullptr) throw Error("no existe " + full);
    if (entry->kind == Entry::Kind::Property) throw Error(full + " no es una funcion");
    Call c(self, args);
    return entry->call(c);
}

Value NativeApi::get(std::string_view key, const Value& self) {
    std::string full;
    const Entry* entry = resolve(key, self, full);
    const Value::Array no_args;
    if (entry != nullptr && entry->kind == Entry::Kind::Property) {
        Call c(self, no_args);
        return entry->call(c);
    }
    if (entry == nullptr) {
        std::string key;
        if (const Dynamic* dyn = dynamicFor(full, key); dyn != nullptr && dyn->get) {
            const Value::Array args{Value(key)};
            Call c(self, args);
            return dyn->get(c);
        }
    }
    throw Error(entry == nullptr ? "no existe " + full : full + " no es una propiedad");
}

void NativeApi::set(std::string_view key, const Value& value, const Value& self) {
    std::string full;
    const Entry* entry = resolve(key, self, full);
    if (entry != nullptr && entry->kind == Entry::Kind::Property) {
        if (!entry->assign) throw Error(full + " es de solo lectura");
        const Value::Array args{value};
        Call c(self, args);
        entry->assign(c);
        return;
    }
    if (entry == nullptr) {
        std::string key;
        if (const Dynamic* dyn = dynamicFor(full, key); dyn != nullptr && dyn->set) {
            const Value::Array args{Value(key), value};
            Call c(self, args);
            dyn->set(c);
            return;
        }
    }
    throw Error(entry == nullptr ? "no existe " + full : full + " no es una propiedad");
}

std::uint64_t NativeApi::handleId(const std::shared_ptr<Handle>& h) {
    if (!h) return 0;
    const auto found = handle_ids_.find(h.get());
    if (found != handle_ids_.end()) return found->second;
    const std::uint64_t id = next_handle_++;
    handles_.emplace(id, h);
    handle_ids_.emplace(h.get(), id);
    return id;
}

std::shared_ptr<Handle> NativeApi::handle(std::uint64_t id) const {
    const auto found = handles_.find(id);
    return found == handles_.end() ? nullptr : found->second;
}

void NativeApi::clearHandles() {
    handles_.clear();
    handle_ids_.clear();
    interned_.clear();
}

nlohmann::json NativeApi::toJson(const Value& v, int depth) {
    using json = nlohmann::json;
    if (depth > 16) return nullptr;
    switch (v.type()) {
        case Value::Type::Nil: return nullptr;
        case Value::Type::Bool: return v.truthy();
        case Value::Type::Number: {
            const double d = v.asNumber();
            return std::isfinite(d) ? json(d) : json(nullptr);
        }
        case Value::Type::String: return v.asString();
        case Value::Type::Vec3: {
            const core::Vec3 p = v.asVec3();
            return json{{"$v", {p.x, p.y, p.z}}};
        }
        case Value::Type::Quat: {
            const core::Quat q = v.asQuat();
            return json{{"$q", {q.x, q.y, q.z, q.w}}};
        }
        case Value::Type::Entity: return json{{"$e", entityId(v.asEntity())}};
        case Value::Type::Function: return json{{"$f", v.callbackId()}};
        case Value::Type::Handle: return json{{"$h", handleId(v.asHandle())}};
        case Value::Type::Array: {
            json a = json::array();
            for (const Value& item : v.items()) a.push_back(toJson(item, depth + 1));
            return a;
        }
        case Value::Type::Object: {
            json o = json::object();
            for (const auto& [k, item] : v.fields()) o[k] = toJson(item, depth + 1);
            return o;
        }
    }
    return nullptr;
}

Value NativeApi::fromJson(const nlohmann::json& j, int depth) {
    if (depth > 16 || j.is_null()) return {};
    if (j.is_boolean()) return Value(j.get<bool>());
    if (j.is_number()) return Value(j.get<double>());
    if (j.is_string()) return Value(j.get<std::string>());
    if (j.is_array()) {
        Value::Array list;
        list.reserve(j.size());
        for (const auto& item : j) list.push_back(fromJson(item, depth + 1));
        return Value(std::move(list));
    }
    if (j.is_object()) {
        if (j.size() == 1) {
            const auto first = j.begin();
            const std::string& key = first.key();
            const nlohmann::json& value = first.value();
            if (key == "$v" && value.is_array() && value.size() >= 3) {
                return Value(core::Vec3{value[0].get<float>(), value[1].get<float>(), value[2].get<float>()});
            }
            if (key == "$q" && value.is_array() && value.size() >= 4) {
                return Value(core::Quat{value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>()});
            }
            if (key == "$e" && value.is_number()) return Value::entity(entityFromId(value.get<std::uint64_t>()));
            if (key == "$f" && value.is_number()) return Value::function(value.get<std::uint64_t>());
            if (key == "$h" && value.is_number()) return Value::handle(handle(value.get<std::uint64_t>()));
        }
        Value o = Value::object();
        for (const auto& [key, item] : j.items()) o.set(key, fromJson(item, depth + 1));
        return o;
    }
    return {};
}

void NativeApi::invoke(const Value& fn, const Value::Array& args) {
    if (fn.type() != Value::Type::Function) return;
    if (fn.callbackId() >= kLocalCallbackBase) {
        if (local_) local_(fn.callbackId(), args);
        return;
    }
    if (!sink_) return;
    nlohmann::json list = nlohmann::json::array();
    for (const Value& a : args) list.push_back(toJson(a));
    sink_(fn.callbackId(), list.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

std::string NativeApi::bridgeCall(const nlohmann::json& request) {
    using json = nlohmann::json;
    const std::string op = request.value("op", std::string("call"));
    const std::string fn = request.value("fn", std::string());
    try {
        Value self;
        if (request.contains("self")) {
            const json& s = request["self"];
            self = fromJson(s);
            // Un handle que ya se solto (al parar el juego) o una entidad nula.
            if (self.isNil() || (self.isEntity() && self.asEntity() == entt::null)) {
                return json{{"ok", false}, {"error", "el objeto ya no existe"}}.dump();
            }
            if (!self.isEntity() && !self.isHandle()) {
                return json{{"ok", false}, {"error", fn + ": el objeto no es del motor"}}.dump();
            }
        }
        const std::string key = op == "call" ? fn : request.value("key", fn);
        if (op == "get") return json{{"ok", true}, {"result", toJson(get(key, self))}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        if (op == "set") {
            set(key, fromJson(request.contains("value") ? request["value"] : json()), self);
            return json{{"ok", true}, {"result", nullptr}}.dump();
        }
        Value::Array args;
        if (request.contains("args") && request["args"].is_array()) {
            for (const json& a : request["args"]) args.push_back(fromJson(a));
        }
        return json{{"ok", true}, {"result", toJson(call(key, args, self))}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (const std::exception& e) {
        const std::string what = e.what();
        // El nombre delante, salvo que el error ya lo lleve ("no existe X").
        const bool named = what.find(fn) != std::string::npos;
        return json{{"ok", false}, {"error", named ? what : fn + ": " + what}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
}

// --- Puente binario ----------------------------------------------------------------
// Lo mismo que bridgeCall sin JSON: el nombre se busca una vez (ApiResolve) y
// los valores van en binario (CppProtocol.h, ValueTag). Antes cada llamada
// armaba y parseaba dos JSON y buscaba el nombre en el indice: decenas de us
// en setQuaternion, playAnimation o setText.

namespace {

using cppproto::ValueTag;

// Hasta 16 niveles, como toJson/fromJson; mas hondo llega nil.
constexpr int kMaxDepth = 16;
// Tope de ids por NativeApi: las claves son nombres de la API, pero un script
// puede armar nombres (propiedades dinamicas); por encima, por el JSON.
constexpr std::size_t kMaxSlots = 1u << 16;

bool documented(const Doc& d) { return !d.args.empty() || !d.description.empty() || !d.returns.empty(); }

// Una funcion o metodo que no devuelve nada (su Doc no dice "devuelve") puede
// ir en el lote del SDK, sin esperar. Las que no tienen Doc, no (no se sabe).
bool batchable(const Entry& e) {
    return e.kind != Entry::Kind::Property && documented(e.doc) && e.doc.returns.empty();
}

}  // namespace

std::uint32_t NativeApi::nextEpoch() {
    static std::atomic<std::uint32_t> next{1};
    std::uint32_t epoch = next.fetch_add(1, std::memory_order_relaxed);
    if (epoch == 0) epoch = next.fetch_add(1, std::memory_order_relaxed);  // 0 = "usa el JSON"
    return epoch;
}

namespace {

class BinaryCodec {
public:
    explicit BinaryCodec(NativeApi& api) : api_(api) {}

    void write(cppproto::Writer& w, const Value& v, int depth = 0) {
        if (depth > kMaxDepth) {
            w.u8(static_cast<std::uint8_t>(ValueTag::Nil));
            return;
        }
        switch (v.type()) {
            case Value::Type::Nil: w.u8(static_cast<std::uint8_t>(ValueTag::Nil)); return;
            case Value::Type::Bool: w.u8(static_cast<std::uint8_t>(v.truthy() ? ValueTag::True : ValueTag::False)); return;
            case Value::Type::Number: {
                const double d = v.asNumber();
                // Como toJson: lo que no es finito llega como nil.
                if (!std::isfinite(d)) {
                    w.u8(static_cast<std::uint8_t>(ValueTag::Nil));
                } else {
                    w.u8(static_cast<std::uint8_t>(ValueTag::Number)).f64(d);
                }
                return;
            }
            case Value::Type::String: w.u8(static_cast<std::uint8_t>(ValueTag::String)).str(v.asString()); return;
            case Value::Type::Vec3: {
                const core::Vec3 p = v.asVec3();
                w.u8(static_cast<std::uint8_t>(ValueTag::Vec3)).f32(p.x).f32(p.y).f32(p.z);
                return;
            }
            case Value::Type::Quat: {
                const core::Quat q = v.asQuat();
                w.u8(static_cast<std::uint8_t>(ValueTag::Quat)).f32(q.x).f32(q.y).f32(q.z).f32(q.w);
                return;
            }
            case Value::Type::Entity: w.u8(static_cast<std::uint8_t>(ValueTag::Entity)).u64(entityId(v.asEntity())); return;
            case Value::Type::Function: w.u8(static_cast<std::uint8_t>(ValueTag::Function)).u64(v.callbackId()); return;
            case Value::Type::Handle: w.u8(static_cast<std::uint8_t>(ValueTag::Handle)).u64(api_.handleId(v.asHandle())); return;
            case Value::Type::Array: {
                const Value::Array& items = v.items();
                w.u8(static_cast<std::uint8_t>(ValueTag::Array)).u32(static_cast<std::uint32_t>(items.size()));
                for (const Value& item : items) write(w, item, depth + 1);
                return;
            }
            case Value::Type::Object: {
                const Value::Object& fields = v.fields();
                w.u8(static_cast<std::uint8_t>(ValueTag::Object)).u32(static_cast<std::uint32_t>(fields.size()));
                for (const auto& [key, item] : fields) {
                    w.str(key);
                    write(w, item, depth + 1);
                }
                return;
            }
        }
        w.u8(static_cast<std::uint8_t>(ValueTag::Nil));
    }

    Value read(cppproto::Reader& r, int depth = 0) {
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
                return Value(core::Vec3{v[0], v[1], v[2]});
            }
            case ValueTag::Quat: {
                core::Quat q;
                q.x = r.f32();
                q.y = r.f32();
                q.z = r.f32();
                q.w = r.f32();
                return Value(q);
            }
            case ValueTag::Entity: return Value::entity(entityFromId(r.u64()));
            case ValueTag::Function: return Value::function(r.u64());
            case ValueTag::Handle: return Value::handle(api_.handle(r.u64()));
            case ValueTag::Array: {
                const std::uint32_t n = r.u32();
                // Cada valor ocupa al menos un byte: un n mayor es un mensaje roto.
                if (!r.ok() || n > r.remaining()) {
                    bad_ = true;
                    return {};
                }
                Value::Array list;
                list.reserve(n);
                for (std::uint32_t i = 0; i < n && r.ok(); ++i) list.push_back(depth < kMaxDepth ? read(r, depth + 1) : skipValue(r));
                return Value(std::move(list));
            }
            case ValueTag::Object: {
                const std::uint32_t n = r.u32();
                if (!r.ok() || n > r.remaining()) {
                    bad_ = true;
                    return {};
                }
                Value o = Value::object();
                for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
                    const std::string key = r.str();
                    o.set(key, depth < kMaxDepth ? read(r, depth + 1) : skipValue(r));
                }
                return o;
            }
            case ValueTag::Json: {
                const nlohmann::json j = nlohmann::json::parse(r.str(), nullptr, false);
                return j.is_discarded() ? Value{} : api_.fromJson(j);
            }
        }
        bad_ = true;  // etiqueta desconocida: no se puede seguir leyendo
        return {};
    }

    bool bad() const { return bad_; }

private:
    // Demasiado hondo: se lee (para seguir con el resto) y llega nil.
    Value skipValue(cppproto::Reader& r) {
        read(r, kMaxDepth);
        return {};
    }
    NativeApi& api_;
    bool bad_ = false;
};

void writeError(std::vector<std::uint8_t>& reply, std::string_view text) {
    cppproto::Writer w;
    w.u32(static_cast<std::uint32_t>(cppproto::ApiStatus::Error)).str(text);
    reply.insert(reply.end(), w.bytes.begin(), w.bytes.end());
}

}  // namespace

std::string NativeApi::bridgeBinary(BridgeOp op, const void* data, std::size_t size, std::vector<std::uint8_t>& reply) {
    cppproto::Reader r(data, size);
    if (op == BridgeOp::Resolve) {
        std::string key = r.str();
        cppproto::Writer w;
        if (!r.ok() || key.empty() || (slots_.size() >= kMaxSlots && !slot_index_.contains(key))) {
            w.u32(0).u32(0).u32(0);  // epoca 0: el SDK usa el JSON
            reply.insert(reply.end(), w.bytes.begin(), w.bytes.end());
            return {};
        }
        std::uint32_t id = 0;
        if (const auto it = slot_index_.find(key); it != slot_index_.end()) {
            id = it->second;
        } else {
            BridgeSlot slot;
            slot.member = key.front() == ':';
            slot.key = slot.member ? key.substr(1) : key;
            if (!slot.member) {
                if (const auto found = index_.find(key); found != index_.end()) slot.entry = found->second;
            }
            id = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back(std::move(slot));
            slot_index_.emplace(std::move(key), id);
        }
        const BridgeSlot& slot = slots_[id];
        // De un tipo que no se sabe hasta ver self: nunca en lote (salvo los set).
        const std::uint32_t flags = slot.entry != SIZE_MAX && batchable(entries_[slot.entry]) ? cppproto::ApiBatchable : 0u;
        w.u32(epoch_).u32(id).u32(flags);
        reply.insert(reply.end(), w.bytes.begin(), w.bytes.end());
        return {};
    }

    const bool send = op == BridgeOp::Send;
    const std::uint32_t epoch = r.u32();
    const std::uint32_t id = r.u32();
    const auto call_op = static_cast<cppproto::ApiOp>(r.u32());
    if (!r.ok()) {
        if (!send) writeError(reply, "peticion binaria rota");
        return send ? "Api: peticion binaria rota" : std::string{};
    }
    if (epoch != epoch_ || id >= slots_.size()) {
        if (send) return "Api: llamada en lote de otra sesion de scripts (se ignora)";
        cppproto::Writer w;
        w.u32(static_cast<std::uint32_t>(cppproto::ApiStatus::Stale));
        reply.insert(reply.end(), w.bytes.begin(), w.bytes.end());
        return {};
    }
    BridgeSlot& slot = slots_[id];
    // El nombre que sale en los errores: el de la peticion JSON ("fn").
    const std::string& shown = slot.key;
    BinaryCodec codec(*this);
    try {
        const Value self = codec.read(r);
        const std::uint32_t count = r.u32();
        if (!r.ok() || codec.bad() || count > r.remaining()) throw Error("peticion binaria rota");
        Value::Array args;
        args.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) args.push_back(codec.read(r));
        if (!r.ok() || codec.bad()) throw Error("peticion binaria rota");

        // self: como en bridgeCall (un handle que ya se solto o una entidad nula).
        const bool has_self = slot.member || slot.key.find(':') != std::string::npos;
        if (has_self) {
            if (self.isNil() || (self.isEntity() && self.asEntity() == entt::null)) throw Error("el objeto ya no existe");
            if (!self.isEntity() && !self.isHandle()) throw Error("el objeto no es del motor");
        }

        // La entrada: fija o, de un tipo, por el duenio de self (con cache del ultimo).
        const Entry* entry = nullptr;
        if (slot.member) {
            const std::string_view owner = self.isEntity() ? std::string_view("Entity") : self.asHandle()->typeName();
            if (owner != slot.owner) {
                slot.owner.assign(owner);
                const auto found = index_.find(slot.owner + ":" + slot.key);
                slot.owner_entry = found != index_.end() ? found->second : SIZE_MAX;
            }
            if (slot.owner_entry != SIZE_MAX) entry = &entries_[slot.owner_entry];
        } else if (slot.entry != SIZE_MAX) {
            entry = &entries_[slot.entry];
        }

        Value result;
        if (entry == nullptr) {
            // Sin entrada registrada: el camino por nombre (propiedades
            // dinamicas como Graphics.post.x y el error "no existe").
            const Value& own = has_self ? self : Value::nil();
            if (call_op == cppproto::ApiOp::Get) {
                result = get(slot.key, own);
            } else if (call_op == cppproto::ApiOp::Set) {
                set(slot.key, args.empty() ? Value::nil() : args.front(), own);
            } else {
                result = call(slot.key, args, own);
            }
        } else if (call_op == cppproto::ApiOp::Call) {
            if (entry->kind == Entry::Kind::Property) throw Error(shown + " no es una funcion");
            Call c(self, args);
            result = entry->call(c);
        } else if (call_op == cppproto::ApiOp::Get) {
            if (entry->kind != Entry::Kind::Property) throw Error(shown + " no es una propiedad");
            const Value::Array no_args;
            Call c(self, no_args);
            result = entry->call(c);
        } else {
            if (entry->kind != Entry::Kind::Property) throw Error(shown + " no es una propiedad");
            if (!entry->assign) throw Error(shown + " es de solo lectura");
            if (args.size() > 1) args.resize(1);
            if (args.empty()) args.emplace_back();
            Call c(self, args);
            entry->assign(c);
        }

        if (send) {
            // En lote no hay a quien devolverlo: su Doc deberia decir "devuelve".
            if (!result.isNil() && call_op == cppproto::ApiOp::Call && !slot.warned) {
                slot.warned = true;
                return "Api: " + shown + " devolvio un valor pero va en lote (su Doc no dice que devuelva nada): " +
                       "anade lo que devuelve a su api::Doc";
            }
            return {};
        }
        cppproto::Writer w;
        w.u32(static_cast<std::uint32_t>(cppproto::ApiStatus::Ok));
        codec.write(w, result);
        reply.insert(reply.end(), w.bytes.begin(), w.bytes.end());
        return {};
    } catch (const std::exception& e) {
        const std::string what = e.what();
        // El nombre delante, salvo que el error ya lo lleve (como bridgeCall).
        const std::string text = what.find(shown) != std::string::npos ? what : shown + ": " + what;
        if (send) return "Api::send: " + text;
        writeError(reply, text);  // (nada se escribio antes en reply)
        return {};
    }
}

}  // namespace cramion::scripting::api
