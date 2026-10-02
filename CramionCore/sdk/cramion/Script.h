// Cramion C++ scripting: lo que incluyen los scripts del juego.
//
//   // Assets/Scripts/Jugador.cpp
//   #include <cramion/Script.h>
//   using namespace cramion;
//
//   class Jugador : public Script {
//   public:
//       // Propiedades: salen en el Inspector con su tipo (como en Unity).
//       Property<float> velocidad{this, "velocidad", 5.0f, Range(0, 20), Tooltip("Metros por segundo")};
//       Property<Prefab> bala{this, "bala"};                 // arrastrar un prefab
//       Property<Entity> objetivo{this, "objetivo"};          // arrastrar un objeto de la escena
//       Property<Entity> luz{this, "luz", {}, Requires("Light")};  // un objeto con Light
//       Property<AudioClip> disparo{this, "disparo"};
//       Property<Color> tinte{this, "tinte", Color(1, 0.5f, 0)};
//       Property<int> modo{this, "modo", 0, Options{"Andar", "Correr", "Volar"}};
//       Property<std::vector<Entity>> puntos{this, "puntos"};
//
//       void update(float dt) override {
//           Vec3 mover{Input::axis("Horizontal"), 0.0f, -Input::axis("Vertical")};
//           entity().translate(mover * (velocidad * dt));
//           if (Input::keyDown("Space")) Scene::instantiate(bala, entity().position());
//           Audio::playOneShot(disparo);                       // toda la API de Lua, en C++
//       }
//   };
//   CRAMION_SCRIPT(Jugador)
//
// Corren en un proceso aparte (CramionScriptHost): si un script falla
// (puntero nulo, excepcion, recursion infinita, bucle sin fin) el motor
// sigue, muestra el error con el archivo y la linea y desactiva ese script.
//
// API: lo escrito aqui (Entity, Scene, Input, Physics, Time, Debug, CVars)
// mas TODA la API de Lua generada en Api.gen.h (Audio, UI, Navigation,
// Network, Graphics, Prefs, Voxel, Mesh, Animator...), con Value como tipo
// de los argumentos y del resultado.
#pragma once

#include "CppProtocol.h"
#include "Types.h"
#include "Value.h"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace cramion {

class Script;

namespace detail {
extern const cppproto::Api* g_api;
extern cppproto::EventArgs g_event;

// Las llamadas sin respuesta se juntan y van en un solo mensaje (antes de la
// siguiente que si espera respuesta y al terminar el evento).
void queue(cppproto::Rpc op, const cppproto::Writer& w);
void flush();

// Una llamada al motor: los argumentos y un lector de la respuesta.
inline cppproto::Reader rpc(cppproto::Rpc op, const cppproto::Writer& w = {}) {
    flush();
    const void* reply = nullptr;
    std::uint32_t size = 0;
    if (g_api != nullptr) size = g_api->rpc(static_cast<std::uint32_t>(op), w.bytes.data(), static_cast<std::uint32_t>(w.bytes.size()), &reply);
    return cppproto::Reader(reply, reply != nullptr ? size : 0);
}

// Puente a la API de Lua (Api.gen.h la usa).
Value luaRequest(const Value& request);
// Los argumentos opcionales que no se pasan llegan como nil: los del final no se envian.
inline Values trimNils(Values args) {
    while (!args.empty() && args.back().isNil()) args.pop_back();
    return args;
}
inline Value lua(std::string_view fn, Values args) {
    args = trimNils(std::move(args));
    return luaRequest(Value{{"op", "call"}, {"fn", fn}, {"args", Value(std::move(args))}});
}
inline Value luaMethod(const Value& self, std::string_view fn, Values args) {
    args = trimNils(std::move(args));
    return luaRequest(Value{{"op", "call"}, {"fn", fn}, {"self", self}, {"args", Value(std::move(args))}});
}
inline Value luaGet(std::string_view fn) { return luaRequest(Value{{"op", "get"}, {"fn", fn}}); }
inline Value luaGetField(const Value& self, std::string_view key) {
    return luaRequest(Value{{"op", "get"}, {"self", self}, {"key", key}});
}
inline void luaSetField(const Value& self, std::string_view key, const Value& v) {
    luaRequest(Value{{"op", "set"}, {"self", self}, {"key", key}, {"value", v}});
}
}  // namespace detail

// --- Debug ---
namespace Debug {
/// Escribe en la Consola (y en la Consola C++).
inline void log(std::string_view text) {
    cppproto::Writer w;
    w.u32(0).str(text);
    detail::rpc(cppproto::Rpc::Log, w);  // al momento: si el script falla despues, el mensaje ya salio
}
/// Aviso (amarillo) en la Consola.
inline void warning(std::string_view text) {
    cppproto::Writer w;
    w.u32(1).str(text);
    detail::rpc(cppproto::Rpc::Log, w);
}
/// Error (rojo) en la Consola.
inline void error(std::string_view text) {
    cppproto::Writer w;
    w.u32(2).str(text);
    detail::rpc(cppproto::Rpc::Log, w);
}
}  // namespace Debug

enum class ForceMode : std::uint32_t { Force = 0, Acceleration = 1, Impulse = 2, VelocityChange = 3 };

// --- Entity ---
class Entity {
public:
    Entity() = default;
    explicit Entity(std::uint64_t id) : id_(id) {}
    /// Identificador de la entidad (0 = ninguna).
    std::uint64_t id() const { return id_; }
    /// Existe todavia (no se ha destruido).
    bool valid() const { return id_ != 0 && u32(cppproto::Rpc::Valid) != 0; }
    explicit operator bool() const { return valid(); }
    bool operator==(const Entity& o) const { return id_ == o.id_; }
    bool operator!=(const Entity& o) const { return id_ != o.id_; }

    /// Nombre del objeto (el de la Jerarquia).
    std::string name() const { return str(cppproto::Rpc::GetName); }
    /// Cambia el nombre del objeto.
    void setName(std::string_view n) const {
        cppproto::Writer w;
        w.u64(id_).str(n);
        detail::queue(cppproto::Rpc::SetName, w);
    }
    /// Su etiqueta (Tag).
    std::string tag() const { return str(cppproto::Rpc::GetTag); }

    /// Posicion en el mundo.
    Vec3 position() const { return vec(cppproto::VecGet::WorldPosition); }
    /// Posicion respecto a su padre.
    Vec3 localPosition() const { return vec(cppproto::VecGet::LocalPosition); }
    /// Giro local en grados (X, Y, Z), como en el Inspector.
    Vec3 rotation() const { return vec(cppproto::VecGet::Euler); }  // grados (locales)
    /// Escala local.
    Vec3 scale() const { return vec(cppproto::VecGet::Scale); }
    /// Su delante en el mundo (-Z local).
    Vec3 forward() const { return vec(cppproto::VecGet::Forward); }
    /// Su derecha en el mundo (+X local).
    Vec3 right() const { return vec(cppproto::VecGet::Right); }
    /// Su arriba en el mundo (+Y local).
    Vec3 up() const { return vec(cppproto::VecGet::Up); }
    /// Pone la posicion en el mundo.
    void setPosition(const Vec3& v) const { set(cppproto::VecSet::WorldPosition, v); }
    /// Pone la posicion respecto a su padre.
    void setLocalPosition(const Vec3& v) const { set(cppproto::VecSet::LocalPosition, v); }
    /// Pone el giro local en grados (X, Y, Z).
    void setRotation(const Vec3& degrees) const { set(cppproto::VecSet::Euler, degrees); }
    /// Pone la escala local.
    void setScale(const Vec3& v) const { set(cppproto::VecSet::Scale, v); }
    /// Mueve d metros (en el mundo).
    void translate(const Vec3& d) const { set(cppproto::VecSet::Translate, d); }
    /// Gira estos grados (X, Y, Z).
    void rotate(const Vec3& degrees) const { set(cppproto::VecSet::Rotate, degrees); }
    /// Gira para mirar a un punto del mundo.
    void lookAt(const Vec3& target) const { set(cppproto::VecSet::LookAt, target); }

    /// Esta activo en la escena.
    bool active() const { return u32(cppproto::Rpc::GetActive) != 0; }
    /// Activa o desactiva el objeto (y sus hijos).
    void setActive(bool on) const {
        cppproto::Writer w;
        w.u64(id_).u32(on ? 1 : 0);
        detail::queue(cppproto::Rpc::SetActive, w);
    }
    /// Su padre en la Jerarquia (no valida si no tiene).
    Entity parent() const {
        cppproto::Writer w;
        w.u64(id_);
        auto r = detail::rpc(cppproto::Rpc::Parent, w);
        return Entity(r.u64());
    }
    /// Lo borra de la escena (al final del frame).
    void destroy() const {
        cppproto::Writer w;
        w.u64(id_);
        detail::queue(cppproto::Rpc::Destroy, w);
    }

    // Componentes por su nombre ("Light", "Rigidbody", "AudioSource"...).
    /// Tiene ese componente ("Light", "Rigidbody", "AudioSource"...).
    bool has(std::string_view component) const { return strU32(cppproto::Rpc::HasComponent, component) != 0; }
    /// Le pone un componente por su nombre.
    bool add(std::string_view component) const { return strU32(cppproto::Rpc::AddComponent, component) != 0; }
    /// Todos los campos de un componente (objeto JSON).
    Value component(std::string_view type) const {
        cppproto::Writer w;
        w.u64(id_).str(type);
        auto r = detail::rpc(cppproto::Rpc::GetComponent, w);
        return Value::parse(r.str());
    }
    /// Cambia varios campos de un componente a la vez.
    bool setComponent(std::string_view type, const Value& fields) const {
        cppproto::Writer w;
        w.u64(id_).str(type).str(fields.toJson());
        auto r = detail::rpc(cppproto::Rpc::SetComponent, w);
        return r.u32() != 0;
    }
    // Un campo ("intensity", "material.friction", "chains[1].pull").
    /// Un campo de un componente: field("Light", "intensity").
    Value field(std::string_view component, std::string_view path) const {
        cppproto::Writer w;
        w.u64(id_).str(component).str(path);
        auto r = detail::rpc(cppproto::Rpc::GetField, w);
        const bool ok = r.u32() != 0;
        const std::string text = r.str();
        if (!ok) return {};
        const Value v = Value::parse(text);
        return v.isNil() && !text.empty() && text != "null" ? Value(text) : v;  // texto sin comillas
    }
    /// Cambia un campo: setField("Light", "intensity", 2.0f).
    bool setField(std::string_view component, std::string_view path, const Value& value) const {
        cppproto::Writer w;
        w.u64(id_).str(component).str(path).str(value.toJson());
        auto r = detail::rpc(cppproto::Rpc::SetField, w);
        return r.u32() != 0;
    }
    /// Un campo numerico de un componente.
    float getFloat(std::string_view component, std::string_view path) const { return field(component, path).asFloat(); }
    /// Un campo verdadero/falso de un componente.
    bool getBool(std::string_view component, std::string_view path) const { return field(component, path).asBool(); }
    /// Cambia un campo numerico de un componente.
    bool setFloat(std::string_view component, std::string_view path, float v) const { return setField(component, path, v); }
    /// Cambia un campo verdadero/falso de un componente.
    bool setBool(std::string_view component, std::string_view path, bool v) const { return setField(component, path, v); }

    // Fisica (Rigidbody o Character Controller).
    /// Velocidad (Rigidbody o Character Controller), m/s.
    Vec3 velocity() const { return vec(cppproto::VecGet::Velocity); }
    /// Pone la velocidad del Rigidbody.
    void setVelocity(const Vec3& v) const { set(cppproto::VecSet::Velocity, v); }
    /// Velocidad de giro del Rigidbody.
    Vec3 angularVelocity() const { return vec(cppproto::VecGet::AngularVelocity); }
    /// Pone la velocidad de giro del Rigidbody.
    void setAngularVelocity(const Vec3& v) const { set(cppproto::VecSet::AngularVelocity, v); }
    /// Empuja el Rigidbody (fuerza, aceleracion, impulso o cambio de velocidad).
    void addForce(const Vec3& f, ForceMode mode = ForceMode::Force) const {
        float v[3] = {f.x, f.y, f.z};
        cppproto::Writer w;
        w.u64(id_).vec3(v).u32(static_cast<std::uint32_t>(mode));
        detail::queue(cppproto::Rpc::AddForce, w);
    }

    // Character Controller.
    /// Character Controller: mueve chocando con el mundo; devuelve con que choco (bits).
    std::uint32_t move(const Vec3& d) const {
        float v[3] = {d.x, d.y, d.z};
        cppproto::Writer w;
        w.u64(id_).vec3(v);
        auto r = detail::rpc(cppproto::Rpc::CharMove, w);
        return r.u32();
    }
    /// Character Controller: esta en el suelo.
    bool isGrounded() const { return u32(cppproto::Rpc::CharGrounded) != 0; }
    /// Character Controller: hacia donde andar (con su velocidad, gravedad y escalones).
    void setMoveInput(const Vec3& dir, bool run = false) const {
        float v[3] = {dir.x, dir.y, dir.z};
        cppproto::Writer w;
        w.u64(id_).vec3(v).u32(run ? 1 : 0);
        detail::queue(cppproto::Rpc::CharInput, w);
    }
    /// Character Controller: salta (altura en metros; -1 = la del componente).
    bool jump(float height = -1.0f) const {
        cppproto::Writer w;
        w.u64(id_).f32(height);
        auto r = detail::rpc(cppproto::Rpc::CharJump, w);
        return r.u32() != 0;
    }
    /// Character Controller: agacharse o levantarse.
    void setCrouch(bool on) const {
        cppproto::Writer w;
        w.u64(id_).u32(on ? 1 : 0);
        detail::queue(cppproto::Rpc::CharCrouch, w);
    }

    // Cualquier metodo o campo de la entidad en Lua (entity:metodo(...), entity.campo).
    /// Cualquier metodo de Lua de la entidad: call("playSound", {"Audio/x.wav"}).
    Value call(std::string_view method, Values args = {}) const { return detail::luaMethod(Value(*this), method, std::move(args)); }
    /// Cualquier campo de Lua de la entidad.
    Value get(std::string_view field_name) const { return detail::luaGetField(Value(*this), field_name); }
    /// Cambia un campo de Lua de la entidad.
    void set(std::string_view field_name, const Value& v) const { detail::luaSetField(Value(*this), field_name, v); }

    /// El script de C++ de tipo T de este objeto (nullptr si no tiene): como GetComponent<T>() de Unity.
    /// auto* marcador = Scene::find("Marcador").script<Marcador>();
    template <class T>
    T* script() const;

    // Metodos de Lua generados (entity:playSound(...), entity:moveTo(...)...).
#include "EntityApi.gen.inc"

private:
    std::uint32_t u32(cppproto::Rpc op) const {
        cppproto::Writer w;
        w.u64(id_);
        auto r = detail::rpc(op, w);
        return r.u32();
    }
    std::uint32_t strU32(cppproto::Rpc op, std::string_view s) const {
        cppproto::Writer w;
        w.u64(id_).str(s);
        auto r = detail::rpc(op, w);
        return r.u32();
    }
    std::string str(cppproto::Rpc op) const {
        cppproto::Writer w;
        w.u64(id_);
        auto r = detail::rpc(op, w);
        return r.str();
    }
    Vec3 vec(cppproto::VecGet which) const {
        cppproto::Writer w;
        w.u64(id_).u32(static_cast<std::uint32_t>(which));
        auto r = detail::rpc(cppproto::Rpc::GetVec, w);
        float v[3] = {0, 0, 0};
        r.vec3(v);
        return {v[0], v[1], v[2]};
    }
    void set(cppproto::VecSet which, const Vec3& value) const {
        float v[3] = {value.x, value.y, value.z};
        cppproto::Writer w;
        w.u64(id_).u32(static_cast<std::uint32_t>(which)).vec3(v);
        detail::queue(cppproto::Rpc::SetVec, w);
    }
    std::uint64_t id_ = 0;
};

inline Value::Value(const Entity& e) : type_(Type::Entity), id_(e.id()) {}
inline Entity Value::asEntity() const { return Entity(type_ == Type::Entity ? id_ : 0); }
inline Value::operator Entity() const { return asEntity(); }
inline Value Value::call(std::string_view method, Values args) const { return detail::luaMethod(*this, method, std::move(args)); }
inline Value Value::get(std::string_view field) const { return detail::luaGetField(*this, field); }
inline void Value::setField(std::string_view field, const Value& v) const { detail::luaSetField(*this, field, v); }

// Bits de Entity::move (lo que toco).
constexpr std::uint32_t kCollidedSides = 1, kCollidedAbove = 2, kCollidedBelow = 4;

struct Collision {
    Entity other;
    Vec3 point;
    Vec3 normal;
    Vec3 relativeVelocity;
};

// --- Referencias a assets (se arrastran en el Inspector) ---
enum class AssetKind : int { Model = 1, Environment = 2, Scene = 3, AnimatorController = 4, AnimationClip = 5, Material = 6,
                             Prefab = 7, RenderTexture = 8, StateMachine = 9 };
template <AssetKind K>
struct AssetRef {
    std::string uuid;
    std::string path;  // dentro de Assets
    /// Existe todavia (no se ha destruido).
    bool valid() const { return !uuid.empty() || !path.empty(); }
    explicit operator bool() const { return valid(); }
    operator Value() const { return Value(path.empty() ? uuid : path); }  // lo que aceptan las funciones de Lua
};
using Model = AssetRef<AssetKind::Model>;
using Material = AssetRef<AssetKind::Material>;
using Prefab = AssetRef<AssetKind::Prefab>;
using SceneAsset = AssetRef<AssetKind::Scene>;
using AnimatorController = AssetRef<AssetKind::AnimatorController>;
using AnimationClip = AssetRef<AssetKind::AnimationClip>;
using RenderTexture = AssetRef<AssetKind::RenderTexture>;
using EnvironmentAsset = AssetRef<AssetKind::Environment>;
using StateMachineAsset = AssetRef<AssetKind::StateMachine>;

// Archivos sueltos de Assets (imagenes, sonidos, scripts de Lua, shaders).
enum class FileKind : int { Texture = 0, Audio = 1, LuaScript = 2, Shader = 3, Any = 4 };
template <FileKind K>
struct FileRef {
    std::string path;
    /// Existe todavia (no se ha destruido).
    bool valid() const { return !path.empty(); }
    explicit operator bool() const { return valid(); }
    operator Value() const { return Value(path); }
};
using Texture = FileRef<FileKind::Texture>;
using AudioClip = FileRef<FileKind::Audio>;
using LuaScript = FileRef<FileKind::LuaScript>;
using ShaderFile = FileRef<FileKind::Shader>;
using AnyFile = FileRef<FileKind::Any>;

// --- Opciones de las propiedades ---
struct Range {
    double min, max;
    Range(double a, double b) : min(a), max(b) {}
};
struct Tooltip {
    const char* text;
    explicit Tooltip(const char* t) : text(t) {}
};
struct Header {
    const char* text;
    explicit Header(const char* t) : text(t) {}
};
struct Label {
    const char* text;
    explicit Label(const char* t) : text(t) {}
};
struct Options {
    std::vector<std::string> names;
    Options(std::initializer_list<const char*> list) {
        for (const char* n : list) names.emplace_back(n);
    }
};
// Una entidad que tenga ese componente ("Light", "Rigidbody"...).
struct Requires {
    const char* component;
    explicit Requires(const char* c) : component(c) {}
};

namespace detail {

struct PropInfo {
    std::string name, label, tooltip, header;
    std::string kind;     // bool, int, float, string, vec2, vec3, color, entity, asset, file, enum, list
    std::string element;  // listas: el tipo de cada elemento
    std::string type;     // asset: tipo; file: clase; entity: componente requerido
    bool has_range = false;
    double min = 0.0, max = 0.0;
    std::vector<std::string> options;
    Value default_value;
};

class PropertyBase {
public:
    PropertyBase(Script* owner, const char* name);
    virtual ~PropertyBase() = default;
    virtual void load(const Value& v) = 0;
    virtual PropInfo info() const = 0;
    const std::string& name() const { return name_; }

protected:
    std::string name_;
    std::string label_, tooltip_, header_, requires_;
    bool has_range_ = false;
    double min_ = 0.0, max_ = 0.0;
    std::vector<std::string> options_;

    void option(const Range& r) {
        has_range_ = true;
        min_ = r.min;
        max_ = r.max;
    }
    void option(const Tooltip& t) { tooltip_ = t.text; }
    void option(const Header& h) { header_ = h.text; }
    void option(const Label& l) { label_ = l.text; }
    void option(const Options& o) { options_ = o.names; }
    void option(const Requires& r) { requires_ = r.component; }
};

// Como se describe y se lee cada tipo.
template <typename T>
struct PropType;
template <>
struct PropType<bool> {
    static const char* kind() { return "bool"; }
    static bool load(const Value& v, bool fb) { return v.isNil() ? fb : v.asBool(); }
    static Value save(bool v) { return v; }
};
template <typename T>
struct PropTypeNumber {
    static const char* kind() { return std::is_integral_v<T> ? "int" : "float"; }
    static T load(const Value& v, T fb) { return v.isNil() ? fb : static_cast<T>(v.asNumber()); }
    static Value save(T v) { return static_cast<double>(v); }
};
template <> struct PropType<int> : PropTypeNumber<int> {};
template <> struct PropType<long long> : PropTypeNumber<long long> {};
template <> struct PropType<unsigned> : PropTypeNumber<unsigned> {};
template <> struct PropType<float> : PropTypeNumber<float> {};
template <> struct PropType<double> : PropTypeNumber<double> {};
template <>
struct PropType<std::string> {
    static const char* kind() { return "string"; }
    static std::string load(const Value& v, const std::string& fb) { return v.isNil() ? fb : v.asString(); }
    static Value save(const std::string& v) { return v; }
};
template <>
struct PropType<Vec2> {
    static const char* kind() { return "vec2"; }
    static Vec2 load(const Value& v, Vec2 fb) { return v.isNil() ? fb : v.asVec2(); }
    static Value save(Vec2 v) { return Vec3{v.x, v.y, 0.0f}; }
};
template <>
struct PropType<Vec3> {
    static const char* kind() { return "vec3"; }
    static Vec3 load(const Value& v, Vec3 fb) { return v.isNil() ? fb : v.asVec3(); }
    static Value save(Vec3 v) { return v; }
};
template <>
struct PropType<Color> {
    static const char* kind() { return "color"; }
    static Color load(const Value& v, Color fb) { return v.isNil() ? fb : v.asColor(); }
    static Value save(Color v) { return v; }
};
template <>
struct PropType<Entity> {
    static const char* kind() { return "entity"; }
    static Entity load(const Value& v, Entity fb) { return v.isNil() ? fb : v.asEntity(); }
    static Value save(Entity) { return {}; }
};
template <AssetKind K>
struct PropType<AssetRef<K>> {
    static const char* kind() { return "asset"; }
    static std::string type() { return std::to_string(static_cast<int>(K)); }
    static AssetRef<K> load(const Value& v, const AssetRef<K>& fb) {
        if (v.isNil()) return fb;
        return AssetRef<K>{v["uuid"].asString(), v["path"].asString()};
    }
    static Value save(const AssetRef<K>& a) { return Value{{"uuid", a.uuid}, {"path", a.path}}; }
};
template <FileKind K>
struct PropType<FileRef<K>> {
    static const char* kind() { return "file"; }
    static std::string type() { return std::to_string(static_cast<int>(K)); }
    static FileRef<K> load(const Value& v, const FileRef<K>& fb) { return v.isNil() ? fb : FileRef<K>{v.asString()}; }
    static Value save(const FileRef<K>& f) { return f.path; }
};
template <typename E>
struct PropTypeEnum {
    static const char* kind() { return "enum"; }
    static E load(const Value& v, E fb) { return v.isNil() ? fb : static_cast<E>(v.asInt()); }
    static Value save(E v) { return static_cast<int>(v); }
};
template <typename U>
struct PropType<std::vector<U>> {
    static const char* kind() { return "list"; }
    static std::vector<U> load(const Value& v, const std::vector<U>& fb) {
        if (!v.isArray()) return fb;
        std::vector<U> out;
        for (const Value& item : v.items()) out.push_back(PropType<U>::load(item, U{}));
        return out;
    }
    static Value save(const std::vector<U>& list) {
        Value out = Value::array();
        for (const U& item : list) out.push(PropType<U>::save(item));
        return out;
    }
};

template <typename T>
struct IsAssetOrFile : std::false_type {};
template <AssetKind K>
struct IsAssetOrFile<AssetRef<K>> : std::true_type {};
template <FileKind K>
struct IsAssetOrFile<FileRef<K>> : std::true_type {};

template <typename T>
using PropTypeOf =std::conditional_t<std::is_enum_v<T>, PropTypeEnum<T>, PropType<T>>;

template <typename T, typename = void>
struct HasTypeName : std::false_type {};
template <typename T>
struct HasTypeName<T, std::void_t<decltype(PropTypeOf<T>::type())>> : std::true_type {};

}  // namespace detail

// Una propiedad del script: sale en el Inspector y el valor que se ponga ahi
// llega al script antes de awake().
template <typename T>
class Property : public detail::PropertyBase {
public:
    template <typename... Opts>
    Property(Script* owner, const char* name, T default_value = T{}, Opts&&... opts)
        : PropertyBase(owner, name), value_(default_value), default_(default_value) {
        (option(std::forward<Opts>(opts)), ...);
    }
    const T& get() const { return value_; }
    T& get() { return value_; }
    operator const T&() const { return value_; }
    const T* operator->() const { return &value_; }
    T* operator->() { return &value_; }
    // Assets y archivos directos a la API: Audio::playOneShot(disparo), Scene::instantiate(bala, ...).
    template <typename U = T, typename = std::enable_if_t<detail::IsAssetOrFile<U>::value>>
    operator Value() const {
        return Value(value_);
    }
    Property& operator=(const T& v) {
        value_ = v;
        return *this;
    }
    // Comodidades para listas.
    template <typename U = T>
    auto operator[](std::size_t i) const -> decltype(std::declval<const U&>()[i]) {
        return value_[i];
    }

    void load(const Value& v) override { value_ = detail::PropTypeOf<T>::load(v, default_); }
    detail::PropInfo info() const override {
        detail::PropInfo p;
        p.name = name_;
        p.label = label_;
        p.tooltip = tooltip_;
        p.header = header_;
        p.kind = detail::PropTypeOf<T>::kind();
        if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
            if (!options_.empty()) p.kind = "enum";
        }
        if constexpr (detail::HasTypeName<T>::value) p.type = detail::PropTypeOf<T>::type();
        if constexpr (std::is_same_v<T, Entity>) p.type = requires_;
        if constexpr (requires { typename T::value_type; } && !std::is_same_v<T, std::string>) {
            using U = typename T::value_type;
            p.element = detail::PropTypeOf<U>::kind();
            if constexpr (detail::HasTypeName<U>::value) p.type = detail::PropTypeOf<U>::type();
            if constexpr (std::is_same_v<U, Entity>) p.type = requires_;
        }
        p.has_range = has_range_;
        p.min = min_;
        p.max = max_;
        p.options = options_;
        p.default_value = detail::PropTypeOf<T>::save(default_);
        return p;
    }

private:
    T value_;
    T default_;
};

// Atajo: CR_PROPERTY(float, velocidad, 5.0f, Range(0, 10))
#define CR_PROPERTY(Type, name, ...) ::cramion::Property<Type> name{this, #name, __VA_ARGS__}

// --- Scene ---
namespace Scene {
/// El primer objeto con ese nombre (no valida si no hay).
inline Entity find(std::string_view name) {
    cppproto::Writer w;
    w.str(name);
    auto r = detail::rpc(cppproto::Rpc::FindEntity, w);
    return Entity(r.u64());
}
/// El primer objeto con esa etiqueta.
inline Entity findWithTag(std::string_view tag) {
    cppproto::Writer w;
    w.str(tag);
    auto r = detail::rpc(cppproto::Rpc::FindWithTag, w);
    return Entity(r.u64());
}
/// Crea un objeto vacio.
inline Entity create(std::string_view name = "Nuevo") {
    cppproto::Writer w;
    w.str(name);
    auto r = detail::rpc(cppproto::Rpc::CreateEntity, w);
    return Entity(r.u64());
}
// En lote (un solo mensaje para muchas entidades).
/// Las posiciones de muchos objetos en una llamada (rapido).
inline std::vector<Vec3> getPositions(const std::vector<Entity>& entities) {
    cppproto::Writer w;
    w.u32(static_cast<std::uint32_t>(entities.size()));
    for (const Entity& e : entities) w.u64(e.id());
    auto r = detail::rpc(cppproto::Rpc::GetPositions, w);
    std::vector<Vec3> out(entities.size());
    for (Vec3& p : out) {
        float v[3];
        r.vec3(v);
        p = {v[0], v[1], v[2]};
    }
    return out;
}
/// Mueve muchos objetos en una llamada (rapido).
inline void setPositions(const std::vector<Entity>& entities, const std::vector<Vec3>& positions) {
    cppproto::Writer w;
    const std::size_t n = entities.size() < positions.size() ? entities.size() : positions.size();
    w.u32(static_cast<std::uint32_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const float v[3] = {positions[i].x, positions[i].y, positions[i].z};
        w.u64(entities[i].id()).vec3(v);
    }
    detail::queue(cppproto::Rpc::SetPositions, w);
}
}  // namespace Scene

// --- Input ---
namespace Input {
inline bool keyMode(std::string_view key, std::uint32_t mode) {
    cppproto::Writer w;
    w.str(key).u32(mode);
    auto r = detail::rpc(cppproto::Rpc::Key, w);
    return r.u32() != 0;
}
/// Tecla mantenida ("W", "Space", "LeftShift"...).
inline bool key(std::string_view k) { return keyMode(k, 0); }      // mantenida ("W", "Space", "LeftShift"...)
/// Tecla pulsada este frame.
inline bool keyDown(std::string_view k) { return keyMode(k, 1); }  // este frame
/// Tecla soltada este frame.
inline bool keyUp(std::string_view k) { return keyMode(k, 2); }
/// Boton del raton (0 izq., 1 der., 2 centro); mode 0 mantenido, 1 pulsado, 2 soltado.
inline bool mouseButton(int button, std::uint32_t mode = 0) {
    cppproto::Writer w;
    w.u32(static_cast<std::uint32_t>(button)).u32(mode);
    auto r = detail::rpc(cppproto::Rpc::MouseButton, w);
    return r.u32() != 0;
}
// "Horizontal" y "Vertical" (teclado, mando y joystick tactil), "Mouse X", "Mouse Y".
/// Eje: "Horizontal", "Vertical" (WASD/flechas/stick), "Mouse X", "Mouse Y".
inline float axis(std::string_view name) {
    cppproto::Writer w;
    w.str(name);
    auto r = detail::rpc(cppproto::Rpc::Axis, w);
    return r.f32();
}
struct MouseState {
    float x, y, dx, dy, wheel;
};
/// Posicion y movimiento del raton.
inline MouseState mouse() {
    auto r = detail::rpc(cppproto::Rpc::Mouse);
    MouseState m{};
    m.x = r.f32();
    m.y = r.f32();
    m.dx = r.f32();
    m.dy = r.f32();
    m.wheel = r.f32();
    return m;
}
}  // namespace Input

// --- Physics ---
struct RaycastHit {
    Entity entity;
    Vec3 point;
    Vec3 normal;
    float distance = 0.0f;
};
namespace Physics {
/// Lanza un rayo; true si choca (hit: punto, normal, distancia y objeto).
inline bool raycast(const Vec3& origin, const Vec3& direction, float max_distance, RaycastHit* hit, Entity ignore = {}) {
    float o[3] = {origin.x, origin.y, origin.z};
    float d[3] = {direction.x, direction.y, direction.z};
    cppproto::Writer w;
    w.vec3(o).vec3(d).f32(max_distance).u64(ignore.id());
    auto r = detail::rpc(cppproto::Rpc::Raycast, w);
    const bool touched = r.u32() != 0;
    if (touched && hit != nullptr) {
        hit->entity = Entity(r.u64());
        float p[3], n[3];
        r.vec3(p);
        r.vec3(n);
        hit->point = {p[0], p[1], p[2]};
        hit->normal = {n[0], n[1], n[2]};
        hit->distance = r.f32();
    }
    return touched;
}
}  // namespace Physics

// --- Time ---
namespace Time {
/// Segundos desde el frame anterior.
inline float deltaTime() { return detail::g_event.delta_time; }
/// Segundos desde que empezo el juego.
inline float time() { return detail::g_event.time; }
/// Paso fijo de la fisica (segundos).
inline float fixedDeltaTime() { return detail::g_event.fixed_delta; }
/// Frames desde que empezo el juego.
inline std::uint64_t frameCount() { return detail::g_event.frame; }
}  // namespace Time

// --- Lua: cualquier cosa de la API por su nombre ---
namespace Lua {
/// Llama a cualquier funcion de Lua por su nombre: Lua::call("Audio.playOneShot", {"x.wav"}).
inline Value call(std::string_view fn, Values args = {}) { return detail::lua(fn, std::move(args)); }
/// Lee cualquier valor de Lua ("Time.time", "MiTabla.valor").
inline Value get(std::string_view path) { return detail::luaGet(path); }
/// Cambia un valor global de Lua.
inline void set(std::string_view path, const Value& v) { detail::luaRequest(Value{{"op", "set"}, {"fn", path}, {"value", v}}); }
// Sin esperar la respuesta (va en el lote: mas rapido).
void send(std::string_view fn, Values args = {});
}  // namespace Lua

// --- CVars ---
namespace detail {
struct CVarDecl {
    std::string name;
    std::uint32_t type;
    std::string default_value;
    std::string description;
    std::uint32_t flags;
};
std::vector<CVarDecl>& pendingCVars();
void registerCVar(const CVarDecl& decl);
}  // namespace detail

enum CVarFlags : std::uint32_t { CVarNone = 0, CVarReadOnly = 1, CVarSaved = 2, CVarCheat = 4 };

// Lee y escribe cualquier CVar del motor (las del motor y las de los scripts).
namespace CVars {
/// El valor de una CVar del motor o del juego, como texto.
inline std::string get(std::string_view name, std::string_view fallback = {}) {
    cppproto::Writer w;
    w.str(name);
    auto r = detail::rpc(cppproto::Rpc::CVarGet, w);
    const bool exists = r.u32() != 0;
    std::string v = r.str();
    return exists ? v : std::string(fallback);
}
/// Cambia una CVar ("r.Shadows 0").
inline bool set(std::string_view name, std::string_view value, std::string* error = nullptr) {
    cppproto::Writer w;
    w.str(name).str(value);
    auto r = detail::rpc(cppproto::Rpc::CVarSet, w);
    const bool ok = r.u32() != 0;
    std::string e = r.str();
    if (!ok && error != nullptr) *error = e;
    return ok;
}
}  // namespace CVars

// Una variable propia del juego: se declara (global o estatica) y se ve en
// la ventana Variables del editor. bool, int, float, double o std::string.
template <typename T>
class CVar {
public:
    CVar(const char* name, T default_value, const char* description = "", std::uint32_t flags = CVarNone)
        : name_(name), default_(default_value) {
        detail::CVarDecl decl{name, typeCode(), format(default_value), description != nullptr ? description : "", flags};
        if (detail::g_api != nullptr) detail::registerCVar(decl);
        else detail::pendingCVars().push_back(decl);  // al cargar la DLL
    }
    T get() const {
        const std::string v = CVars::get(name_, format(default_));
        if constexpr (std::is_same_v<T, std::string>) return v;
        else if constexpr (std::is_same_v<T, bool>) return v == "true" || v == "1";
        else if constexpr (std::is_integral_v<T>) return static_cast<T>(std::atoll(v.c_str()));
        else return static_cast<T>(std::atof(v.c_str()));
    }
    operator T() const { return get(); }
    bool set(const T& value) const { return CVars::set(name_, format(value)); }
    CVar& operator=(const T& value) {
        set(value);
        return *this;
    }
    const char* name() const { return name_; }

private:
    static std::uint32_t typeCode() {
        if constexpr (std::is_same_v<T, bool>) return 0;
        else if constexpr (std::is_integral_v<T>) return 1;
        else if constexpr (std::is_floating_point_v<T>) return 2;
        else return 3;
    }
    static std::string format(const T& v) {
        if constexpr (std::is_same_v<T, std::string>) return v;
        else if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
        else return std::to_string(v);
    }
    const char* name_;
    T default_;
};

// --- Script ---
class Script {
public:
    virtual ~Script() = default;
    Entity entity() const { return entity_; }

    // Lo que puso el Inspector, por nombre (las Property<T> ya lo tienen).
    Value property(const std::string& name) const {
        const auto it = values_.find(name);
        return it != values_.end() ? it->second : Value();
    }
    template <typename T>
    T property(const std::string& name, T fallback) const {
        const Value v = property(name);
        if (v.isNil()) return fallback;
        if constexpr (std::is_same_v<T, const char*>) return fallback;
        else return static_cast<T>(v);
    }
    std::string property(const std::string& name, const char* fallback) const {
        const Value v = property(name);
        return v.isNil() ? std::string(fallback) : v.asString();
    }

    /// Al crearse el script (antes de start).
    virtual void awake() {}
    /// Una vez, antes del primer update.
    virtual void start() {}
    /// Cada frame (dt = segundos desde el anterior).
    virtual void update(float /*dt*/) {}
    /// Cada paso de la fisica.
    virtual void fixedUpdate(float /*dt*/) {}
    /// Cada frame, despues de todos los update (camaras).
    virtual void lateUpdate(float /*dt*/) {}
    /// Al destruirse el objeto o parar el juego.
    virtual void onDestroy() {}
    /// Empieza un choque con otro objeto.
    virtual void onCollisionEnter(const Collision&) {}
    /// Sigue el choque.
    virtual void onCollisionStay(const Collision&) {}
    /// Termina el choque.
    virtual void onCollisionExit(const Collision&) {}
    /// Otro objeto entra en un trigger.
    virtual void onTriggerEnter(Entity /*other*/) {}
    /// Otro objeto sigue dentro del trigger.
    virtual void onTriggerStay(Entity /*other*/) {}
    /// Otro objeto sale del trigger.
    virtual void onTriggerExit(Entity /*other*/) {}

    /// Un mensaje por su nombre: los botones (on_click "OnJugar"), sliders, casillas y campos de la UI
    /// lo envian al script del objeto. Por defecto llama al de on(...).
    virtual void onMessage(const std::string& name, const Value& value) {
        if (name == "OnOriginShift") onOriginShift(value);
        if (name == "OnNetVar") onNetVar(value["key"].asString(), value["value"]);
        const auto it = handlers_.find(name);
        if (it != handlers_.end()) it->second(value);
    }
    /// Mundos grandes: el motor recoloco el origen y todo se movio -offset (resta offset a las posiciones guardadas).
    virtual void onOriginShift(const Vec3& /*offset*/) {}
    /// Objeto de red: llego un cambio de una variable sincronizada (setNetVar del dueno o del servidor).
    virtual void onNetVar(const std::string& /*key*/, const Value& /*value*/) {}
    /// Que hacer con un mensaje (en el constructor o en awake):
    /// on("OnJugar", [this](const Value&) { Scene::load("Nivel1"); });
    /// on("OnVolumen", [this](const Value& v) { Prefs::setFloat("volumen", v); });
    void on(const std::string& name, std::function<void(const Value&)> fn) { handlers_[name] = std::move(fn); }

private:
    friend struct ScriptAccess;
    friend class detail::PropertyBase;
    Entity entity_;
    std::map<std::string, Value> values_;
    std::vector<detail::PropertyBase*> properties_;
    std::map<std::string, std::function<void(const Value&)>> handlers_;
};

namespace detail {
// Los scripts vivos de este proceso (para Entity::script<T>()).
inline std::vector<Script*>& liveScripts() {
    static std::vector<Script*> list;
    return list;
}
}  // namespace detail

template <class T>
T* Entity::script() const {
    for (Script* s : detail::liveScripts()) {
        if (s->entity().id() != id_) continue;
        if (T* t = dynamic_cast<T*>(s)) return t;
    }
    return nullptr;
}

inline detail::PropertyBase::PropertyBase(Script* owner, const char* name) : name_(name) {
    if (owner != nullptr) owner->properties_.push_back(this);
}

namespace detail {
using Factory = Script* (*)();
void registerClass(const char* name, const char* file, Factory factory);
template <typename T>
struct Registrar {
    Registrar(const char* name, const char* file) {
        registerClass(name, file, []() -> Script* { return new T(); });
    }
};
}  // namespace detail

}  // namespace cramion

// Una por clase, en su .cpp: la hace visible al componente "C++ Script".
#define CRAMION_SCRIPT(Class) static ::cramion::detail::Registrar<Class> cramion_registrar_##Class(#Class, __FILE__);

// Toda la API de Lua (Audio, UI, Navigation, Network...) en C++.
#include "Api.gen.h"
