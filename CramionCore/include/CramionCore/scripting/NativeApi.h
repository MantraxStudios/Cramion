#ifndef CRAMION_CORE_SCRIPTING_NATIVE_API_H
#define CRAMION_CORE_SCRIPTING_NATIVE_API_H

// La API de scripting del motor, en C++ (sin Lua): cada funcion
// ("Audio.playOneShot"), metodo ("Entity:translate") y propiedad
// ("Time.deltaTime", "Entity:name") es una funcion de C++ que recibe Values y
// devuelve un Value.
//
//   api.function("Debug.log", [](api::Call& c) { ...; return api::Value{}; },
//                {"...", "Escribe en la consola"});
//   api.method("Entity", "translate", [](api::Call& c) { c.entity(); c.vec3(0); ... });
//
// La usan los scripts de C++ (bridgeCall: el mismo JSON que el SDK, ver
// sdk/cramion/Value.h) y, mientras dure la migracion, tambien Lua (cada
// entrada se publica en el estado de Lua): asi cada funcion se escribe una
// sola vez. Ver PLAN-SIN-LUA.md.

#include <CramionFX/core/Math.h>

#include <entt/entity/entity.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cramion::scripting::api {

// Un objeto del motor que vive mientras alguien lo tenga (una malla, una
// peticion HTTP...). Sus metodos y propiedades se registran con su typeName().
class Handle {
public:
    virtual ~Handle() = default;
    virtual std::string_view typeName() const = 0;
};

class Value {
public:
    enum class Type : std::uint8_t { Nil, Bool, Number, String, Vec3, Quat, Entity, Array, Object, Function, Handle };
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;  // en el orden en que se anaden

    Value() = default;
    Value(std::nullptr_t) {}
    template <typename T>
    Value(T*) = delete;  // un puntero no es un bool
    Value(bool b) : type_(Type::Bool), number_(b ? 1.0 : 0.0) {}
    Value(int v) : type_(Type::Number), number_(v) {}
    Value(unsigned v) : type_(Type::Number), number_(v) {}
    Value(long long v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(unsigned long long v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(float v) : type_(Type::Number), number_(v) {}
    Value(double v) : type_(Type::Number), number_(v) {}
    Value(const char* s) : type_(Type::String), text_(s != nullptr ? s : "") {}
    Value(std::string s) : type_(Type::String), text_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), text_(s) {}
    Value(const core::Vec3& v) : type_(Type::Vec3), v_{v.x, v.y, v.z, 0.0f} {}
    Value(const core::Quat& q) : type_(Type::Quat), v_{q.x, q.y, q.z, q.w} {}
    Value(Array list) : type_(Type::Array), array_(std::make_shared<Array>(std::move(list))) {}

    static Value entity(entt::entity e);
    static Value object();
    static Value function(std::uint64_t callback_id);
    static Value handle(std::shared_ptr<Handle> h);

    Type type() const { return type_; }
    bool isNil() const { return type_ == Type::Nil; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    // nil y false son falso; lo demas, verdadero.
    bool truthy() const { return type_ != Type::Nil && !(type_ == Type::Bool && number_ == 0.0); }

    double asNumber(double fallback = 0.0) const;
    std::string asString() const;  // texto legible de cualquier valor (Debug.log)
    core::Vec3 asVec3() const { return {v_[0], v_[1], v_[2]}; }
    core::Quat asQuat() const { return {v_[0], v_[1], v_[2], v_[3]}; }
    entt::entity asEntity() const { return type_ == Type::Entity ? entity_ : entt::entity{entt::null}; }
    std::uint64_t callbackId() const { return type_ == Type::Function ? id_ : 0; }
    const std::shared_ptr<Handle>& asHandle() const;
    template <typename T>
    std::shared_ptr<T> as() const { return std::dynamic_pointer_cast<T>(asHandle()); }

    // Listas (desde 0) y objetos.
    std::size_t size() const;
    const Value& operator[](std::size_t i) const;
    const Value& operator[](std::string_view key) const;
    void push(Value v);
    void set(std::string_view key, Value v);
    const Array& items() const;
    const Object& fields() const;

    static const Value& nil();

private:
    Type type_ = Type::Nil;
    double number_ = 0.0;
    std::string text_;
    float v_[4] = {0, 0, 0, 0};
    entt::entity entity_ = entt::null;
    std::uint64_t id_ = 0;
    std::shared_ptr<Array> array_;
    std::shared_ptr<Object> object_;
    std::shared_ptr<Handle> handle_;
};

// Un error de la llamada (argumento que falta, objeto que ya no existe...):
// vuelve al script como error, no tumba el motor.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Los argumentos de una llamada, con conversiones comprobadas (los indices
// empiezan en 0 y no cuentan `self`).
class Call {
public:
    Call(const Value& self, const Value::Array& args) : self_(self), args_(args) {}

    const Value& self() const { return self_; }
    std::size_t count() const { return args_.size(); }
    const Value& arg(std::size_t i) const { return i < args_.size() ? args_[i] : Value::nil(); }
    bool has(std::size_t i) const { return !arg(i).isNil(); }

    double number(std::size_t i) const;
    double number(std::size_t i, double fallback) const;
    bool boolean(std::size_t i, bool fallback = false) const;
    std::string string(std::size_t i) const;
    std::string string(std::size_t i, std::string_view fallback) const;
    core::Vec3 vec3(std::size_t i) const;
    core::Vec3 vec3(std::size_t i, const core::Vec3& fallback) const;
    entt::entity entity(std::size_t i) const;  // entt::null si es nil
    entt::entity selfEntity() const;           // el `self` de un metodo de Entity
    const Value& function(std::size_t i) const;  // nil o una funcion

private:
    [[noreturn]] void wrong(std::size_t i, const char* expected) const;
    const Value& self_;
    const Value::Array& args_;
};

using Function = std::function<Value(Call&)>;

struct Doc {
    std::string args;         // "clip, volumen" (para el autocompletado y Api.gen.h)
    std::string description;  // una linea
};

struct Entry {
    enum class Kind : std::uint8_t { Function, Method, Property };
    Kind kind = Kind::Function;
    std::string owner;  // "Audio" (tabla) o "Entity" (tipo)
    std::string name;
    Function call;    // funcion, metodo o lectura de la propiedad
    Function assign;  // escritura de la propiedad (vacia = solo lectura); el valor es arg(0)
    Doc doc;
    bool member() const { return kind == Kind::Method || (kind == Kind::Property && type_member); }
    bool type_member = false;  // de un tipo ("Entity:name") y no de una tabla ("Time.deltaTime")
};

class NativeApi {
public:
    // Primer id de los handles nativos (los de Lua van por debajo mientras convivan).
    static constexpr std::uint64_t kHandleBase = std::uint64_t(1) << 40;
    // Primer id de los callbacks del mismo proceso (funciones de Lua; en
    // Android, los scripts de C++): van al invocador local, no al sumidero.
    static constexpr std::uint64_t kLocalCallbackBase = std::uint64_t(1) << 41;

    // --- Registro ---
    void function(std::string path, Function fn, Doc doc = {});  // "Audio.playOneShot"
    void method(std::string type, std::string name, Function fn, Doc doc = {});
    // De una tabla ("Time", "deltaTime") o de un tipo (type_member = true).
    void property(std::string owner, std::string name, Function get, Function set = {}, Doc doc = {},
                  bool type_member = false);

    // "Audio.playOneShot", "Entity:translate", "Time.deltaTime".
    const Entry* find(std::string_view key) const;
    const std::vector<Entry>& entries() const { return entries_; }

    // --- Puente con los scripts de C++ ---
    // La peticion del SDK ({"op","fn","self","key","args","value"}). nullopt si
    // no es de esta API (la atiende Lua mientras dure la migracion).
    std::optional<std::string> bridgeCall(const nlohmann::json& request);
    // Donde van las llamadas a los callbacks de los scripts (id, args JSON).
    void setCallbackSink(std::function<void(std::uint64_t, const std::string&)> sink) { sink_ = std::move(sink); }
    void setLocalInvoker(std::function<void(std::uint64_t, const Value::Array&)> invoker) { local_ = std::move(invoker); }
    void invoke(const Value& fn, const Value::Array& args);

    nlohmann::json toJson(const Value& v, int depth = 0);
    Value fromJson(const nlohmann::json& j, int depth = 0);

    // Objetos del motor que los scripts tienen (se sueltan al parar el juego).
    std::uint64_t handleId(const std::shared_ptr<Handle>& h);
    std::shared_ptr<Handle> handle(std::uint64_t id) const;
    void clearHandles();

private:
    void add(Entry e);
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> index_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Handle>> handles_;
    std::unordered_map<const Handle*, std::uint64_t> handle_ids_;
    std::uint64_t next_handle_ = kHandleBase;
    std::function<void(std::uint64_t, const std::string&)> sink_;
    std::function<void(std::uint64_t, const Value::Array&)> local_;
};

}  // namespace cramion::scripting::api

#endif  // CRAMION_CORE_SCRIPTING_NATIVE_API_H
