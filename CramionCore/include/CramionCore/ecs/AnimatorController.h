#ifndef CRAMION_CORE_ECS_ANIMATOR_CONTROLLER_H
#define CRAMION_CORE_ECS_ANIMATOR_CONTROLLER_H

// Animator Controller (como el de Unity): una maquina de estados que elige
// que clip suena en un componente Animator. Se guarda como asset
// .cranimator (JSON con UUID) y se edita en la ventana Animator del editor.
//
//   Parametros  float / int / bool / trigger (los cambia el juego o el editor)
//   Estados     un clip cada uno (por nombre del modelo o un .cranim), con
//               velocidad y bucle
//   Transiciones de un estado (o de "Cualquier estado") a otro cuando se
//               cumplen TODAS sus condiciones y, si se pide, al llegar a un
//               punto del clip (exit time, normalizado 0..1)
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

struct AnimatorState {
    std::string name = "Estado";
    std::string clip_name;           // clip del modelo por nombre
    assets::AssetRef clip{{}, assets::AssetType::AnimationClip};  // o un .cranim (manda si es valido)
    float speed = 1.0f;
    bool loop = true;
    core::Vec2 position{};           // en el grafo del editor
};

// Origen de una transicion desde "Cualquier estado".
inline constexpr int kAnyState = -1;

struct AnimatorTransition {
    int from = 0;  // indice de estado o kAnyState
    int to = 0;
    bool has_exit_time = false;
    float exit_time = 1.0f;  // normalizado (1 = al terminar el clip)
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
};

// Avanza la maquina: aplica la primera transicion que se cumpla (consume los
// triggers de sus condiciones). `clip_duration` es la del clip del estado
// actual (0 si no tiene). Devuelve true si cambio de estado.
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
