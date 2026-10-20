// Cramion C++ scripting: el formato de los mensajes entre el motor, el
// proceso de los scripts (CramionScriptHost.exe) y la DLL de los scripts.
// Solo C++ estandar: lo comparten el motor y el SDK que compilan los juegos.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace cramion::cppproto {

// Sube si cambia algo de este archivo (la DLL y el motor deben coincidir).
// Los eventos nuevos van al final: una DLL anterior ignora los que no conoce.
constexpr std::uint32_t kAbiVersion = 2;

// Eventos de un script (los metodos de cramion::Script).
enum class Event : std::uint32_t {
    Awake = 0,
    Start,
    Update,
    FixedUpdate,
    LateUpdate,
    Destroy,
    CollisionEnter,
    CollisionStay,
    CollisionExit,
    TriggerEnter,
    TriggerStay,
    TriggerExit,
    Enable,   // onEnable(): tras awake() y cada vez que se reactiva el script o su objeto
    Disable,  // onDisable(): al desactivarse (la instancia sigue viva) y antes de onDestroy si estaba activo
    // Solo pone el tiempo (Time::deltaTime, time, frameCount) sin llamar a nada:
    // el motor lo envia cuando ningun script sobrescribe update/lateUpdate y no
    // hubo viaje (los callbacks y corrutinas leen Time::). Una DLL anterior lo
    // trata igual (evento desconocido = no llama a nada).
    Time,
    Count
};

// Llamadas de los scripts al motor (cada una es un mensaje y su respuesta).
enum class Rpc : std::uint32_t {
    Log = 1,          // u32 nivel (0 info, 1 aviso, 2 error), str
    FindEntity,       // str nombre -> u64
    FindWithTag,      // str tag -> u64
    CreateEntity,     // str nombre -> u64
    Destroy,          // u64
    Valid,            // u64 -> u32
    GetName,          // u64 -> str
    SetName,          // u64 str
    GetTag,           // u64 -> str
    GetVec,           // u64 u32 (VecGet) -> vec3
    SetVec,           // u64 u32 (VecSet) vec3
    GetActive,        // u64 -> u32
    SetActive,        // u64 u32
    Parent,           // u64 -> u64
    HasComponent,     // u64 str -> u32
    AddComponent,     // u64 str -> u32
    GetComponent,     // u64 str -> str (JSON)
    SetComponent,     // u64 str str(JSON con los campos a cambiar) -> u32
    GetField,         // u64 str(componente) str(campo, "a.b") -> u32 ok, str (JSON del valor)
    SetField,         // u64 str str str(JSON del valor) -> u32
    Key,              // str nombre, u32 modo (0 pulsada, 1 recien pulsada, 2 recien soltada) -> u32
    MouseButton,      // u32 boton, u32 modo -> u32
    Axis,             // str -> f32
    Mouse,            // -> f32 x, y, dx, dy, rueda
    Raycast,          // vec3 origen, vec3 dir, f32 max, u64 ignorar -> u32 toca, u64, vec3 punto, vec3 normal, f32 dist
    CharMove,         // u64 vec3 -> u32 banderas
    CharGrounded,     // u64 -> u32
    CharInput,        // u64 vec3 u32 correr
    CharJump,         // u64 f32 altura -> u32
    CharCrouch,       // u64 u32
    CVarGet,          // str -> u32 existe, str valor
    CVarSet,          // str str -> u32 ok, str error
    CVarRegister,     // str nombre, u32 tipo (0 bool,1 int,2 float,3 texto), str defecto, str descripcion, u32 banderas -> u32 ok
    AddForce,         // u64 vec3 u32 modo (0 fuerza, 1 aceleracion, 2 impulso, 3 cambio de velocidad)
    Api,              // str JSON {"op":"call|get|set","fn":"Audio.playOneShot","self":v,"key":"k","args":[...],"value":v} -> str JSON {"ok","result"|"error"}
    ApiSend,          // igual sin respuesta (va en lote)
    Batch,            // u32 n, (u32 rpc, u32 tamano, datos) x n: llamadas sin respuesta juntas
    GetPositions,     // u32 n, u64 x n -> vec3 x n (posicion en el mundo)
    SetPositions,     // u32 n, (u64, vec3) x n
    // La API por indice y en binario (en vez de Api/ApiSend con JSON y nombres):
    ApiResolve,       // str clave ("Audio.playOneShot", "Entity:translate", ":apply" = del tipo de self) -> u32 epoca, u32 id, u32 banderas (ApiFlags); epoca 0 = usar el JSON
    ApiCall,          // u32 epoca, u32 id, u32 ApiOp, valor self, u32 n, valor x n -> u32 ApiStatus, valor | str error
    ApiCallSend,      // igual sin respuesta (va en lote): set de propiedades y funciones sin resultado
    SetTransforms,    // u32 n, u32 TransformMask, (u64, [vec3 pos], [f32 x4 rot local], [vec3 escala local]) x n
    Count
};

// La API binaria (ApiResolve / ApiCall).
enum class ApiOp : std::uint32_t { Call = 0, Get = 1, Set = 2 };
enum class ApiStatus : std::uint32_t { Ok = 0, Error = 1, Stale = 2 };  // Stale: epoca de otro ScriptSystem (resolver otra vez)
enum ApiFlags : std::uint32_t { ApiBatchable = 1 };                     // la funcion no devuelve nada: puede ir en lote
// Valores: u8 etiqueta y su contenido. Json: un str con el JSON del SDK (lo
// que no tiene forma binaria, como los callbacks del script).
enum class ValueTag : std::uint8_t {
    Nil = 0, False, True, Number /* f64 */, String /* str */, Vec3 /* f32 x3 */, Quat /* f32 x4 */, Entity /* u64 */,
    Function /* u64 */, Handle /* u64 */, Array /* u32 n, valor x n */, Object /* u32 n, (str, valor) x n */, Json /* str */
};
// SetTransforms: que lleva cada entidad. LocalPosition: la posicion es local (si no, del mundo).
enum TransformMask : std::uint32_t { TransformPosition = 1, TransformRotation = 2, TransformScale = 4, TransformLocalPosition = 8 };

enum class VecGet : std::uint32_t { WorldPosition = 0, LocalPosition, Euler, Scale, Forward, Right, Up, Velocity, AngularVelocity };
enum class VecSet : std::uint32_t { WorldPosition = 0, LocalPosition, Euler, Scale, Velocity, AngularVelocity, Translate, Rotate, LookAt };

// Datos de un evento (los rellena el motor).
struct EventArgs {
    float delta_time = 0.0f;
    float time = 0.0f;
    float fixed_delta = 0.0f;
    std::uint64_t frame = 0;
    std::uint64_t other = 0;  // colisiones y triggers
    float point[3] = {0, 0, 0};
    float normal[3] = {0, 0, 0};
    float relative_velocity[3] = {0, 0, 0};
};

// Lo que el proceso da a la DLL: una sola puerta para todas las llamadas.
// rpc() devuelve el tamano de la respuesta; *reply apunta a ella hasta la
// siguiente llamada.
struct Api {
    std::uint32_t abi;
    std::uint32_t (*rpc)(std::uint32_t op, const void* data, std::uint32_t size, const void** reply);
};

// --- Serializacion (little endian, sin alinear) ---
class Writer {
public:
    std::vector<std::uint8_t> bytes;
    Writer& u8(std::uint8_t v) {
        bytes.push_back(v);
        return *this;
    }
    Writer& f64(double v) { return raw(&v, sizeof(v)); }
    Writer& u32(std::uint32_t v) { return raw(&v, sizeof(v)); }
    Writer& u64(std::uint64_t v) { return raw(&v, sizeof(v)); }
    Writer& f32(float v) { return raw(&v, sizeof(v)); }
    Writer& vec3(const float* v) { return f32(v[0]).f32(v[1]).f32(v[2]); }
    Writer& str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        return raw(s.data(), s.size());
    }
    Writer& raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
        return *this;
    }
};

class Reader {
public:
    Reader(const void* data, std::size_t size) : p_(static_cast<const std::uint8_t*>(data)), end_(p_ + size) {}
    bool ok() const { return ok_; }
    std::size_t remaining() const { return static_cast<std::size_t>(end_ - p_); }
    std::uint8_t u8() { return get<std::uint8_t>(); }
    double f64() { return get<double>(); }
    std::uint32_t u32() { return get<std::uint32_t>(); }
    std::uint64_t u64() { return get<std::uint64_t>(); }
    float f32() { return get<float>(); }
    void vec3(float* out) {
        out[0] = f32();
        out[1] = f32();
        out[2] = f32();
    }
    // n bytes seguidos (nullptr si no hay tantos).
    const std::uint8_t* bytes(std::size_t n) {
        if (!ok_ || n > remaining()) {
            ok_ = false;
            return nullptr;
        }
        const std::uint8_t* at = p_;
        p_ += n;
        return at;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!ok_ || n > remaining()) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }

private:
    template <typename T>
    T get() {
        T v{};
        if (remaining() < sizeof(T)) {
            ok_ = false;
            return v;
        }
        std::memcpy(&v, p_, sizeof(T));
        p_ += sizeof(T);
        return v;
    }
    const std::uint8_t* p_;
    const std::uint8_t* end_;
    bool ok_ = true;
};

}  // namespace cramion::cppproto
