// Cramion C++ scripting: Value, un valor dinamico (como los de Lua) para la
// API generada (Api.gen.h): nil, bool, numero, texto, Vec3, Quat, entidad,
// lista, objeto, funcion (callback) u objeto del motor (Handle: una malla, una
// maquina de estados...). Se convierte solo a los tipos de C++:
//
//   float volumen = Audio::getVolume();
//   Value hit = Physics::raycast(origen, dir, 50.0f);
//   if (hit) Debug::log(hit["entity"].name());
//   UI::onClick(boton, [](const Values&) { Debug::log("pulsado"); });
//
// Las listas y objetos se comparten al copiar (como las tablas de Lua).
#pragma once

#include "Types.h"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cramion {

class Entity;
class Value;
using Values = std::vector<Value>;
using Callback = std::function<void(const Values&)>;

namespace detail {
std::uint64_t registerCallback(const std::shared_ptr<Callback>& fn);
}

class Value {
public:
    enum class Type : std::uint8_t { Nil, Bool, Number, String, Vec3, Quat, Entity, Array, Object, Function, Handle };
    using Object = std::vector<std::pair<std::string, Value>>;

    Value() = default;
    Value(std::nullptr_t) {}
    // Un puntero no es un Value (sin esto acabaria como bool).
    template <typename T>
    Value(T*) = delete;
    Value(char* s) : Value(static_cast<const char*>(s)) {}
    Value(bool b) : type_(Type::Bool), number_(b ? 1.0 : 0.0) {}
    Value(int v) : type_(Type::Number), number_(v) {}
    Value(unsigned v) : type_(Type::Number), number_(v) {}
    Value(long v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(long long v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(unsigned long long v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(float v) : type_(Type::Number), number_(v) {}
    Value(double v) : type_(Type::Number), number_(v) {}
    Value(const char* s) : type_(Type::String), text_(s != nullptr ? s : "") {}
    Value(std::string s) : type_(Type::String), text_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), text_(s) {}
    Value(const Vec3& v) : type_(Type::Vec3), v_{v.x, v.y, v.z, 0.0f} {}
    Value(const Vec2& v) : type_(Type::Vec3), v_{v.x, v.y, 0.0f, 0.0f} {}
    Value(const Quat& q) : type_(Type::Quat), v_{q.x, q.y, q.z, q.w} {}
    Value(const Color& c) : type_(Type::Vec3), v_{c.r, c.g, c.b, c.a} {}
    Value(const Entity& e);  // Script.h
    Value(Values list) : type_(Type::Array), array_(std::make_shared<Values>(std::move(list))) {}
    Value(std::initializer_list<std::pair<const char*, Value>> fields) : type_(Type::Object), object_(std::make_shared<Object>()) {
        for (const auto& f : fields) object_->emplace_back(f.first, f.second);
    }
    Value(Callback fn) : type_(Type::Function), function_(std::make_shared<Callback>(std::move(fn))) {}
    template <typename F, typename = std::enable_if_t<std::is_invocable_v<F, const Values&> &&
                                                      !std::is_convertible_v<F, std::string_view> && !std::is_same_v<std::decay_t<F>, Value>>>
    Value(F fn) : Value(Callback(std::move(fn))) {}

    static Value array() { return Value(Values{}); }
    static Value object() {
        Value v;
        v.type_ = Type::Object;
        v.object_ = std::make_shared<Object>();
        return v;
    }
    static Value entityId(std::uint64_t id) {
        Value v;
        v.type_ = Type::Entity;
        v.id_ = id;
        return v;
    }
    static Value handle(std::uint64_t id) {
        Value v;
        v.type_ = Type::Handle;
        v.id_ = id;
        return v;
    }

    Type type() const { return type_; }
    bool isNil() const { return type_ == Type::Nil; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }
    bool isHandle() const { return type_ == Type::Handle; }

    // Como en Lua: nil y false son falso; lo demas, verdadero.
    bool truthy() const { return type_ != Type::Nil && !(type_ == Type::Bool && number_ == 0.0); }
    bool asBool(bool fallback = false) const { return type_ == Type::Bool ? number_ != 0.0 : (type_ == Type::Nil ? fallback : truthy()); }
    double asNumber(double fallback = 0.0) const {
        if (type_ == Type::Number || type_ == Type::Bool) return number_;
        if (type_ == Type::String) {
            double d = fallback;
            std::from_chars(text_.data(), text_.data() + text_.size(), d);
            return d;
        }
        return fallback;
    }
    float asFloat(float fallback = 0.0f) const { return static_cast<float>(asNumber(fallback)); }
    int asInt(int fallback = 0) const { return static_cast<int>(asNumber(fallback)); }
    std::string asString(std::string_view fallback = {}) const {
        if (type_ == Type::String) return text_;
        if (type_ == Type::Nil) return std::string(fallback);
        if (type_ == Type::Number) return formatNumber(number_);
        if (type_ == Type::Bool) return number_ != 0.0 ? "true" : "false";
        return toJson();
    }
    Vec3 asVec3() const { return type_ == Type::Vec3 || type_ == Type::Quat ? Vec3{v_[0], v_[1], v_[2]} : Vec3{}; }
    Vec2 asVec2() const { return {v_[0], v_[1]}; }
    Quat asQuat() const { return type_ == Type::Quat ? Quat{v_[0], v_[1], v_[2], v_[3]} : Quat{}; }
    Color asColor() const { return type_ == Type::Vec3 ? Color{v_[0], v_[1], v_[2], v_[3] == 0.0f ? 1.0f : v_[3]} : Color{}; }
    std::uint64_t id() const { return type_ == Type::Entity || type_ == Type::Handle ? id_ : 0; }
    Entity asEntity() const;  // Script.h

    operator bool() const { return truthy(); }
    operator double() const { return asNumber(); }
    operator float() const { return asFloat(); }
    operator int() const { return asInt(); }
    operator std::string() const { return asString(); }
    operator Vec3() const { return asVec3(); }
    operator Vec2() const { return asVec2(); }
    operator Quat() const { return asQuat(); }
    operator Color() const { return asColor(); }
    operator Entity() const;  // Script.h

    // Listas (indice desde 0) y objetos (clave).
    std::size_t size() const {
        return type_ == Type::Array ? array_->size() : (type_ == Type::Object ? object_->size() : 0);
    }
    const Value& operator[](std::size_t i) const { return type_ == Type::Array && i < array_->size() ? (*array_)[i] : nil(); }
    const Value& operator[](int i) const { return i < 0 ? nil() : (*this)[static_cast<std::size_t>(i)]; }
    const Value& operator[](std::string_view key) const {
        if (type_ != Type::Object) return nil();
        for (const auto& [k, v] : *object_) {
            if (k == key) return v;
        }
        return nil();
    }
    const Value& operator[](const char* key) const { return (*this)[std::string_view(key)]; }
    bool has(std::string_view key) const { return !(*this)[key].isNil(); }
    void push(Value v) {
        if (type_ != Type::Array) *this = array();
        array_->push_back(std::move(v));
    }
    void set(std::string_view key, Value v) {
        if (type_ != Type::Object) *this = object();
        for (auto& [k, old] : *object_) {
            if (k == key) {
                old = std::move(v);
                return;
            }
        }
        object_->emplace_back(std::string(key), std::move(v));
    }
    const Values& items() const { return type_ == Type::Array ? *array_ : emptyArray(); }
    const Object& fields() const { return type_ == Type::Object ? *object_ : emptyObject(); }

    // Objetos del motor (Handle) y entidades: metodos y campos (Script.h).
    Value call(std::string_view method, Values args = {}) const;
    Value get(std::string_view field) const;
    void setField(std::string_view field, const Value& v) const;

    // JSON (el formato con el motor).
    std::string toJson() const {
        std::string out;
        write(out);
        return out;
    }
    static Value parse(std::string_view json) {
        std::size_t i = 0;
        return read(json, i);
    }

    static const Value& nil() {
        static const Value n;
        return n;
    }

private:
    static const Values& emptyArray() {
        static const Values a;
        return a;
    }
    static const Object& emptyObject() {
        static const Object o;
        return o;
    }
    static std::string formatNumber(double d) {
        char buffer[40];
        if (d == static_cast<double>(static_cast<long long>(d)) && d > -1e15 && d < 1e15) {
            const auto r = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<long long>(d));
            return std::string(buffer, r.ptr);
        }
        const auto r = std::to_chars(buffer, buffer + sizeof(buffer), d);
        return std::string(buffer, r.ptr);
    }
    static void writeString(std::string& out, std::string_view s) {
        out += '"';
        for (const char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char u[8];
                        const char* hex = "0123456789abcdef";
                        u[0] = '\\';
                        u[1] = 'u';
                        u[2] = '0';
                        u[3] = '0';
                        u[4] = hex[(c >> 4) & 15];
                        u[5] = hex[c & 15];
                        out.append(u, 6);
                    } else {
                        out += c;
                    }
            }
        }
        out += '"';
    }
    void write(std::string& out) const {
        switch (type_) {
            case Type::Nil: out += "null"; break;
            case Type::Bool: out += number_ != 0.0 ? "true" : "false"; break;
            case Type::Number: out += formatNumber(number_); break;
            case Type::String: writeString(out, text_); break;
            case Type::Vec3:
                out += "{\"$v\":[" + formatNumber(v_[0]) + "," + formatNumber(v_[1]) + "," + formatNumber(v_[2]) + "]}";
                break;
            case Type::Quat:
                out += "{\"$q\":[" + formatNumber(v_[0]) + "," + formatNumber(v_[1]) + "," + formatNumber(v_[2]) + "," +
                       formatNumber(v_[3]) + "]}";
                break;
            case Type::Entity: out += "{\"$e\":" + std::to_string(id_) + "}"; break;
            case Type::Handle: out += "{\"$h\":" + std::to_string(id_) + "}"; break;
            case Type::Function: out += "{\"$f\":" + std::to_string(detail::registerCallback(function_)) + "}"; break;
            case Type::Array: {
                out += '[';
                for (std::size_t i = 0; i < array_->size(); ++i) {
                    if (i > 0) out += ',';
                    (*array_)[i].write(out);
                }
                out += ']';
                break;
            }
            case Type::Object: {
                out += '{';
                bool first = true;
                for (const auto& [k, v] : *object_) {
                    if (!first) out += ',';
                    first = false;
                    writeString(out, k);
                    out += ':';
                    v.write(out);
                }
                out += '}';
                break;
            }
        }
    }
    static void skip(std::string_view s, std::size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }
    static std::string readString(std::string_view s, std::size_t& i) {
        std::string out;
        ++i;  // "
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                const char e = s[i++];
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'u': {
                        unsigned code = 0;
                        for (int k = 0; k < 4 && i < s.size(); ++k, ++i) {
                            const char h = s[i];
                            code = code * 16 + static_cast<unsigned>(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                        }
                        // UTF-8 (sin pares sustitutos: basta para el motor).
                        if (code < 0x80) {
                            out += static_cast<char>(code);
                        } else if (code < 0x800) {
                            out += static_cast<char>(0xC0 | (code >> 6));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (code >> 12));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        continue;
                    }
                    default: c = e; break;
                }
            }
            out += c;
        }
        ++i;  // "
        return out;
    }
    static Value read(std::string_view s, std::size_t& i) {
        skip(s, i);
        if (i >= s.size()) return {};
        const char c = s[i];
        if (c == 'n') {
            i += 4;
            return {};
        }
        if (c == 't') {
            i += 4;
            return Value(true);
        }
        if (c == 'f') {
            i += 5;
            return Value(false);
        }
        if (c == '"') return Value(readString(s, i));
        if (c == '[') {
            ++i;
            Value list = array();
            skip(s, i);
            if (i < s.size() && s[i] == ']') {
                ++i;
                return list;
            }
            while (i < s.size()) {
                list.array_->push_back(read(s, i));
                skip(s, i);
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                ++i;  // ]
                break;
            }
            return list;
        }
        if (c == '{') {
            ++i;
            Value obj = object();
            skip(s, i);
            if (i < s.size() && s[i] == '}') {
                ++i;
                return obj;
            }
            while (i < s.size()) {
                skip(s, i);
                std::string key = readString(s, i);
                skip(s, i);
                ++i;  // :
                Value v = read(s, i);
                obj.object_->emplace_back(std::move(key), std::move(v));
                skip(s, i);
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                ++i;  // }
                break;
            }
            // Tipos especiales.
            if (obj.object_->size() == 1) {
                const auto& [k, v] = obj.object_->front();
                if (k == "$v" && v.size() >= 3) return Value(Vec3{v[0].asFloat(), v[1].asFloat(), v[2].asFloat()});
                if (k == "$q" && v.size() >= 4) return Value(Quat{v[0].asFloat(), v[1].asFloat(), v[2].asFloat(), v[3].asFloat()});
                if (k == "$e") return entityId(static_cast<std::uint64_t>(v.asNumber()));
                if (k == "$h") return handle(static_cast<std::uint64_t>(v.asNumber()));
            }
            return obj;
        }
        // Numero.
        std::size_t end = i;
        while (end < s.size() && (s[end] == '-' || s[end] == '+' || s[end] == '.' || s[end] == 'e' || s[end] == 'E' ||
                                  (s[end] >= '0' && s[end] <= '9'))) {
            ++end;
        }
        double d = 0.0;
        std::from_chars(s.data() + i, s.data() + end, d);
        i = end == i ? i + 1 : end;
        return Value(d);
    }

    Type type_ = Type::Nil;
    double number_ = 0.0;
    std::string text_;
    float v_[4] = {0, 0, 0, 0};
    std::uint64_t id_ = 0;
    std::shared_ptr<Values> array_;
    std::shared_ptr<Object> object_;
    std::shared_ptr<Callback> function_;
};

}  // namespace cramion
