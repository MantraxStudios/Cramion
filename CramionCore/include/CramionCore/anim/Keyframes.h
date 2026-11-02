#ifndef CRAMION_CORE_ANIM_KEYFRAMES_H
#define CRAMION_CORE_ANIM_KEYFRAMES_H

// Animacion por fotogramas clave de cualquier propiedad, como las Actions y
// F-Curves de Blender:
//
//   AnimationCurve     una curva (F-Curve): un canal de una propiedad de un
//                      componente ("Transform" / "position", eje x) de una
//                      entidad, con sus claves. Entre clave y clave:
//                      constante, lineal o Bezier con asas (automaticas,
//                      vector, alineadas o libres). Antes de la primera y
//                      despues de la ultima: constante, lineal o en ciclo.
//   KeyframeAnimation  componente con las curvas (la "Action") y como suena
//                      en Play: rango, bucle, velocidad.
//   KeyframeSystem     las hace sonar en Play; en el editor se evalua en el
//                      fotograma de la linea de tiempo (applyAt).
//
// La propiedad se nombra con la misma ruta que usa el Inspector: los grupos
// por su etiqueta, las listas por su clave e indice ("Lente/fov",
// "waypoints/2/position"). Sirve cualquier campo float, int, bool, enum, Vec2
// o Vec3 que describa reflect(); un componente nuevo es animable sin mas.
//
// El tiempo va en segundos; el editor lo muestra en fotogramas (fps).

#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cramion::anim {

enum class Interpolation : int { Constant = 0, Linear = 1, Bezier = 2 };

// Tipo de asa (como Blender). Las automaticas se recalculan al mover claves.
enum class HandleType : int {
    AutoClamped = 0,  // automatica sin pasarse de los valores vecinos (la de Blender por defecto)
    Auto = 1,         // automatica (suave, puede pasarse)
    Vector = 2,       // apunta a la clave vecina (esquina)
    Aligned = 3,      // libre en longitud, alineada con la otra asa
    Free = 4,         // libre del todo
};

enum class Extrapolation : int {
    Constant = 0,  // se queda en el valor de la primera / ultima clave
    Linear = 1,    // sigue la pendiente de los extremos
    Cycle = 2,     // repite las claves (como el modificador Cycles)
};

struct Keyframe {
    float time = 0.0f;   // segundos
    float value = 0.0f;
    Interpolation interpolation = Interpolation::Bezier;  // hacia la siguiente clave
    HandleType left_type = HandleType::AutoClamped;
    HandleType right_type = HandleType::AutoClamped;
    // Asas en coordenadas absolutas (tiempo, valor).
    core::Vec2 left{};
    core::Vec2 right{};
    // Estado del editor (no se guarda).
    bool selected = false;
    bool left_selected = false;
    bool right_selected = false;
};

struct AnimationCurve {
    Uuid target{};           // entidad animada; invalida = la del componente
    std::string component;   // nombre registrado ("Transform")
    std::string property;    // ruta del campo ("position", "Lente/fov")
    int channel = 0;         // eje de un Vec2/Vec3 (0 = x)
    Extrapolation extrapolation = Extrapolation::Constant;
    bool muted = false;      // no se aplica
    std::vector<Keyframe> keys;  // ordenadas por tiempo

    float evaluate(float time) const;
    // Inserta (o sustituye si ya hay una en ese tiempo) y devuelve su indice.
    int insert(float time, float value, Interpolation interpolation = Interpolation::Bezier);
    int find(float time, float epsilon = 1e-4f) const;
    // Tras mover o editar claves: ordena y recalcula las asas automaticas.
    void sort();
    void recalcHandles();
    bool sameChannel(const AnimationCurve& o) const {
        return target == o.target && channel == o.channel && component == o.component && property == o.property;
    }
};

enum class LoopMode : int { Once = 0, Loop = 1, PingPong = 2 };

struct KeyframeAnimation {
    std::vector<AnimationCurve> curves;
    float start = 0.0f;  // segundos
    float end = 4.0f;
    float fps = 24.0f;   // fotogramas del editor (ajuste de las claves)
    bool play_on_start = true;
    LoopMode loop = LoopMode::Loop;
    float speed = 1.0f;
    // Sube (con un contador global: dos estados distintos nunca comparten
    // numero) cada vez que cambia QUE anima alguna curva: anadir o quitar
    // curvas, cambiar su objetivo, componente, propiedad o canal. El sistema
    // guarda en cache a que campo escribe cada curva y la rehace al verla
    // cambiar. Las claves y `muted` no cuentan (se leen cada frame). No se
    // guarda en la escena. Quien toque `curves` a mano llama a touchCurves().
    std::uint64_t curves_version = 0;
    void touchCurves();

    AnimationCurve* findCurve(const Uuid& target, std::string_view component, std::string_view property,
                              int channel);
    AnimationCurve& curve(const Uuid& target, std::string_view component, std::string_view property, int channel);
    // Tiempo de la animacion tras `elapsed` segundos de Play (con el bucle).
    float timeAt(float elapsed) const;

    void reflect(ecs::PropertyVisitor& v);
};

// --- Acceso a propiedades por ruta ---------------------------------------------

enum class PropertyKind : int { Float, Int, Bool, Enum, Vec2, Vec3 };

// Una propiedad animable de un componente (para el menu "Anadir propiedad").
struct AnimatableProperty {
    std::string path;
    std::string label;  // "Lente / Campo de vision"
    PropertyKind kind = PropertyKind::Float;
    int channels = 1;
};

std::vector<AnimatableProperty> animatableProperties(ecs::World& world, ecs::Entity entity,
                                                     std::string_view component);
// Lee un canal; false si la entidad no tiene el componente o el campo.
bool readProperty(ecs::World& world, ecs::Entity entity, std::string_view component, std::string_view path,
                  int channel, float& value);
// Numero de canales del campo (0 si no existe o no es animable).
int propertyChannels(ecs::World& world, ecs::Entity entity, std::string_view component, std::string_view path);

struct ChannelValue {
    std::string_view path;
    int channel = 0;
    float value = 0.0f;
};
// Escribe varios canales de un componente de una vez (una sola pasada de
// reflect(); avisa al componente si algo cambio, p. ej. el Transform).
bool writeProperties(ecs::World& world, ecs::Entity entity, std::string_view component,
                     const std::vector<ChannelValue>& values);

// Codificacion compacta de las claves (asi se guardan en la escena).
std::string encodeKeys(const std::vector<Keyframe>& keys);
std::vector<Keyframe> decodeKeys(std::string_view text);

// --- Sistema ------------------------------------------------------------------

class KeyframeSystem {
public:
    // En Play: avanza las animaciones que suenan y las aplica.
    void update(ecs::World& world, float delta_seconds);
    // Olvida el estado (al entrar o salir de Play).
    void reset() { states_.clear(); }

    void play(ecs::Entity entity, float from = -1.0f);  // -1 = desde su inicio
    void stop(ecs::Entity entity);
    bool isPlaying(ecs::Entity entity) const;
    float time(ecs::Entity entity) const;

    // Aplica las curvas de una animacion en `time` (vista previa del editor).
    static void applyAt(ecs::World& world, ecs::Entity entity, const KeyframeAnimation& animation, float time);
    // Todas las del mundo en `time`.
    static void applyAllAt(ecs::World& world, float time);

    // Para las pruebas: cuantas veces se resolvieron las curvas de una
    // animacion (rutas -> campos) desde el principio.
    std::uint64_t bindCount() const { return bind_count_; }
    std::size_t stateCount() const { return states_.size(); }

private:
    // Cache de lo que escribe cada curva (en Play): se resuelve una vez por
    // reflect() (ruta -> desplazamiento en bytes dentro del componente) y cada
    // frame se escribe directamente en el componente. Antes, por animacion y
    // frame: vectores, world.find, buscar el tipo por nombre y un reflect()
    // que construia la ruta de cada campo como texto.
    struct Binding {
        std::uint32_t curve = 0;    // indice en KeyframeAnimation::curves
        std::uint32_t offset = 0;   // bytes desde el inicio del componente
        PropertyKind kind = PropertyKind::Float;  // Vec2/Vec3: un float suelto
        float fmin = 0.0f, fmax = 0.0f;  // Float: limites (min == max: sin limites)
        int imin = 0, imax = 0;          // Int: limites; Enum: 0..imax
    };
    struct Group {  // un componente de un objeto
        Uuid target{};              // invalido = la entidad de la animacion
        entt::entity handle = entt::null;
        std::size_t type = 0;       // indice en ComponentRegistry::types()
        bool transform = false;     // Transform: sin rehacer el giro si no cambia `rotation`
        std::uint32_t euler_offset = 0;
        std::vector<Binding> bindings;
        // Curvas que no se pueden escribir directamente (campos de listas o
        // copiados a una variable en reflect, rutas que no existen): por la
        // via lenta (writeProperties), como antes.
        std::vector<std::uint32_t> slow;
    };
    struct Cache {
        bool valid = false;
        bool complete = true;           // false: falto algun objetivo o componente
        std::uint64_t curves_version = 0;
        std::size_t curve_count = 0;
        std::uint64_t structure = 0;    // World::structureVersion de los handles
        std::vector<Group> groups;
    };
    struct State {
        Uuid uuid{};  // otra entidad con el mismo handle (otra escena): de cero
        float elapsed = 0.0f;
        bool playing = false;
        bool started = false;
        Cache cache;
    };
    State& stateOf(ecs::Entity entity);
    void bind(ecs::World& world, ecs::Entity entity, const KeyframeAnimation& animation, Cache& cache);
    void applyCached(ecs::World& world, ecs::Entity entity, const KeyframeAnimation& animation, Cache& cache,
                     float time);
    std::unordered_map<entt::entity, State> states_;
    std::uint64_t bind_count_ = 0;
    std::uint64_t pruned_structure_ = ~0ull;  // World::structureVersion de la ultima poda
    // Reutilizados entre frames.
    std::vector<ChannelValue> slow_values_;
    std::vector<const void*> changed_fields_;
};

}  // namespace cramion::anim

#endif  // CRAMION_CORE_ANIM_KEYFRAMES_H
