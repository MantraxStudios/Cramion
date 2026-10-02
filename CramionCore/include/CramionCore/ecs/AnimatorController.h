#ifndef CRAMION_CORE_ECS_ANIMATOR_CONTROLLER_H
#define CRAMION_CORE_ECS_ANIMATOR_CONTROLLER_H

// Animator Controller (como el de Unity): una maquina de estados que elige
// que clip suena en un componente Animator. Se guarda como asset
// .cranimator (JSON con UUID) y se edita en la ventana Animator del editor.
//
//   Parametros  float / int / bool / trigger (los cambia el juego o el editor)
//   Estados     un clip (por nombre del modelo o un .cranim) o un Blend Tree,
//               con velocidad y bucle
//   Blend Trees mezclan varios clips segun parametros float: 1D (andar ->
//               trotar -> correr con "velocidad") o 2D libre (moverse en 8
//               direcciones con "x" e "y"), con los ciclos sincronizados
//   Transiciones de un estado (o de "Cualquier estado") a otro cuando se
//               cumplen TODAS sus condiciones y, si se pide, al llegar a un
//               punto del clip (exit time, normalizado 0..1); con fundido
//               (duracion en segundos) entre la pose vieja y la nueva
//
// Los clips extraidos de un modelo se guardan como .cranim (AnimationClip):
// sus pistas van por NOMBRE de nodo, asi sirven para cualquier modelo con el
// mismo esqueleto.

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/AssetTypes.h"

#include <CramionFX/asset/Model.h>
#include <CramionFX/core/Math.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::ecs {

enum class AnimatorParameterType : int { Float = 0, Int = 1, Bool = 2, Trigger = 3 };

struct AnimatorParameter {
    std::string name;
    AnimatorParameterType type = AnimatorParameterType::Float;
    float default_value = 0.0f;  // bool/trigger: 0 o 1
};

enum class AnimatorConditionMode : int {
    If = 0,        // bool/trigger verdadero
    IfNot = 1,     // bool falso
    Greater = 2,   // float/int
    Less = 3,
    Equals = 4,    // int
    NotEquals = 5,
};

struct AnimatorCondition {
    std::string parameter;
    AnimatorConditionMode mode = AnimatorConditionMode::If;
    float threshold = 0.0f;
};

// Que suena en un estado.
enum class AnimatorMotion : int {
    Clip = 0,         // un clip
    BlendTree1D = 1,  // mezcla por un parametro (umbral de cada clip)
    BlendTree2D = 2,  // mezcla por dos parametros (posicion de cada clip), libre cartesiano
};

// Un clip de un Blend Tree.
struct BlendTreeChild {
    std::string clip_name;  // clip del modelo por nombre
    assets::AssetRef clip{{}, assets::AssetType::AnimationClip};  // o un .cranim (manda si es valido)
    float threshold = 0.0f;  // 1D: valor del parametro en que suena solo este
    core::Vec2 position{};   // 2D: punto (x, y) en que suena solo este
    float speed = 1.0f;      // multiplica la velocidad de este clip
};

struct AnimatorState {
    std::string name = "Estado";
    std::string clip_name;           // clip del modelo por nombre
    assets::AssetRef clip{{}, assets::AssetType::AnimationClip};  // o un .cranim (manda si es valido)
    float speed = 1.0f;
    bool loop = true;
    core::Vec2 position{};           // en el grafo del editor
    // Blend Tree (motion != Clip): los clips de `children` en vez de `clip`.
    AnimatorMotion motion = AnimatorMotion::Clip;
    std::string blend_parameter;     // 1D, y la X del 2D
    std::string blend_parameter_y;   // la Y del 2D
    std::vector<BlendTreeChild> children;

    bool isBlendTree() const { return motion != AnimatorMotion::Clip; }
};

// Peso de cada hijo de un Blend Tree para los valores (x, y) de sus
// parametros (y solo en 2D). Suman 1; vacio si no tiene hijos.
//   1D: entre los dos umbrales vecinos, lineal; fuera, el extremo.
//   2D libre cartesiano (Gradient Band, como Unity): cada hijo pesa segun lo
//       cerca que esta el punto de el respecto a cada uno de los demas.
std::vector<float> blendTreeWeights(const AnimatorState& state, float x, float y = 0.0f);

// Origen de una transicion desde "Cualquier estado".
inline constexpr int kAnyState = -1;

struct AnimatorTransition {
    int from = 0;  // indice de estado o kAnyState
    int to = 0;
    bool has_exit_time = false;
    float exit_time = 1.0f;  // normalizado (1 = al terminar el clip)
    // Fundido: segundos en que la pose pasa del estado viejo al nuevo (0 =
    // corte seco, como antes de la 0.7).
    float duration = 0.0f;
    // Inercial (por defecto, como Unreal): pasa en seco a la pose nueva y
    // apaga poco a poco la diferencia conservando el impulso de cada hueso.
    // Si no, fundido cruzado entre las dos poses (el de Unity).
    bool inertial = true;
    // El estado nuevo sigue en el punto del ciclo del viejo (andar -> correr:
    // el mismo pie delante) en vez de empezar desde el principio.
    bool sync_phase = false;
    std::vector<AnimatorCondition> conditions;
};

struct AnimatorController {
    Uuid uuid{};
    std::vector<AnimatorParameter> parameters;
    std::vector<AnimatorState> states;
    std::vector<AnimatorTransition> transitions;
    int default_state = 0;
    core::Vec2 entry_position{-260.0f, 0.0f};
    core::Vec2 any_state_position{-260.0f, 120.0f};

    const AnimatorParameter* findParameter(const std::string& name) const;
    // Quita un estado y arregla los indices de transiciones y el de defecto.
    void removeState(int index);
};

// .cranimator. Un controlador nuevo recibe UUID nuevo al guardarse si no
// tiene. Los errores van a `error` (si no es nulo).
bool loadAnimatorController(const std::filesystem::path& path, AnimatorController& out,
                            std::string* error = nullptr);
bool saveAnimatorController(const AnimatorController& controller, const std::filesystem::path& path,
                            std::string* error = nullptr);

// Estado de la maquina en una entidad (lo guarda el componente Animator, no
// se serializa).
struct AnimatorRuntime {
    int state = -1;               // -1 = aun no entro (ira al de defecto)
    float state_time = 0.0f;      // segundos en el estado actual
    std::unordered_map<std::string, float> values;  // parametros actuales
    int last_transition = -1;     // la que causo el ultimo cambio (-1 = entrada)
    // Repeticiones (replay/Replay.h): el estado y el tiempo los pone la
    // repeticion; la maquina no avanza ni cambia de estado sola.
    bool replay_control = false;
    // CrossFade del juego (entity:crossFade("Golpe", 0.1)): salta a ese estado
    // por su nombre sin transicion, con fundido o inercializacion. Se consume
    // al aplicarse; el mismo estado vuelve a empezar.
    std::string cross_fade;
    float cross_fade_time = 0.15f;
    bool cross_fade_inertial = true;
    std::string state_name;  // nombre del estado que suena (lo pone RenderSync)
};

// Avanza la maquina: aplica la primera transicion que se cumpla (consume los
// triggers de sus condiciones). `clip_duration` es la del clip del estado
// actual (0 si no tiene; en un Blend Tree, la de la mezcla). Devuelve true si
// cambio de estado (runtime.last_transition dice por cual).
bool stepAnimatorController(const AnimatorController& controller, AnimatorRuntime& runtime,
                            float clip_duration);

float animatorParameterValue(const AnimatorController& controller, const AnimatorRuntime& runtime,
                             const std::string& name);

// --- Clips sueltos (.cranim) -------------------------------------------------

// Guarda un clip de un modelo con las pistas por nombre de nodo. La primera
// linea del archivo es la cabecera (UUID, nombre, duracion) para que la base
// de datos no lea todo el clip.
bool saveAnimationClip(const asset::ModelData& model, const asset::AnimationClip& clip,
                       const std::filesystem::path& path, std::string* error = nullptr);

// Clip con las pistas reasignadas a los nodos de `model` (por nombre; las de
// nodos que no existan se descartan). false si no se pudo leer.
bool loadAnimationClip(const std::filesystem::path& path, const asset::ModelData& model,
                       asset::AnimationClip& out, std::string* error = nullptr);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_ANIMATOR_CONTROLLER_H
