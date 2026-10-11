#include "CramionCore/scripting/NativeApi.h"

#include <entt/entity/entity.hpp>
#include <nlohmann/json.hpp>

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

// Igual que el SDK y el puente de Lua: la entidad viaja como su handle + 1 (0 = ninguna).
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
        std::from_chars(text_.data(), text_.data() + text_.size(), d);
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
        const std::size_t cut = full.find_last_of(".:");
        if (cut != std::string::npos) {
            const auto dyn = dynamic_.find(full.substr(0, cut));
            if (dyn != dynamic_.end() && dyn->second.get) {
                const Value::Array args{Value(full.substr(cut + 1))};
                Call c(self, args);
                return dyn->second.get(c);
            }
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
        const std::size_t cut = full.find_last_of(".:");
        if (cut != std::string::npos) {
            const auto dyn = dynamic_.find(full.substr(0, cut));
            if (dyn != dynamic_.end() && dyn->second.set) {
                const Value::Array args{Value(full.substr(cut + 1)), value};
                Call c(self, args);
                dyn->second.set(c);
                return;
            }
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
    sink_(fn.callbackId(), list.dump());
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
        if (op == "get") return json{{"ok", true}, {"result", toJson(get(key, self))}}.dump();
        if (op == "set") {
            set(key, fromJson(request.contains("value") ? request["value"] : json()), self);
            return json{{"ok", true}, {"result", nullptr}}.dump();
        }
        Value::Array args;
        if (request.contains("args") && request["args"].is_array()) {
            for (const json& a : request["args"]) args.push_back(fromJson(a));
        }
        return json{{"ok", true}, {"result", toJson(call(key, args, self))}}.dump();
    } catch (const std::exception& e) {
        const std::string what = e.what();
        // El nombre delante, salvo que el error ya lo lleve ("no existe X").
        const bool named = what.find(fn) != std::string::npos;
        return json{{"ok", false}, {"error", named ? what : fn + ": " + what}}.dump();
    }
}

}  // namespace cramion::scripting::api
