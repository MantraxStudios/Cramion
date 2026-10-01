#ifndef CRAMION_CORE_AI_STATE_MACHINE_H
#define CRAMION_CORE_AI_STATE_MACHINE_H

// Maquinas de estados para la IA (como los State Graphs de Bolt / Visual
// Scripting de Unity): estados con su propio codigo Lua, transiciones con
// condiciones y una pizarra (blackboard) de variables con tipo.
//
// Asset .crfsm (JSON con UUID):
//   Variables    bool / int / float / string / entity / vec3 con su valor
//                inicial; cada objeto puede cambiarlo en el Inspector
//   Estados      nombre, color, posicion en el grafo y su codigo Lua (o un
//                .lua de Assets): OnEnter(self, sm), OnUpdate(self, dt),
//                OnExit(self), OnLateUpdate, OnFixedUpdate y los de fisica
//                (OnCollisionEnter, OnTriggerEnter...) mientras esta activo
//   Cualquier estado  su codigo corre SIEMPRE antes de mirar las transiciones
//                (sensores: distancia al jugador, vision...) y sus
//                transiciones salen de cualquier estado (morir, huir...)
//   Transiciones de -> a cuando se cumplen TODAS sus condiciones:
//                comparar una variable (con un valor o con otra variable,
//                "$nombre"), un trigger (sm:trigger), un temporizador
//                ("despues de N segundos en el estado") o una expresion Lua.
//                Mayor prioridad primero; a igual prioridad, el orden.
//
// En Play cada objeto con el componente StateMachine tiene su maquina. Desde
// Lua: self.vars.x, sm:get/set, sm:go("Perseguir"), sm:trigger("ruido"),
// entity:getStateMachine() (ver Scripting.h).

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/AssetTypes.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cramion::ecs {
class PropertyVisitor;
}

namespace cramion::ai {

inline constexpr const char* kStateMachineExtension = ".crfsm";

// --- Pizarra -------------------------------------------------------------------

enum class VarType : int { Bool = 0, Int = 1, Float = 2, String = 3, Entity = 4, Vector = 5 };
inline constexpr int kVarTypeCount = 6;
// Nombres del archivo ("bool", "int"...) y para mostrar.
const char* varTypeKey(VarType type);
const char* varTypeLabel(VarType type);
VarType varTypeFromKey(const std::string& key, VarType fallback = VarType::Float);

inline constexpr std::uint32_t kNoEntity = 0xffffffffu;

struct Value {
    VarType type = VarType::Float;
    bool b = false;
    double n = 0.0;        // int y float
    std::string s;         // string; entity: nombre, tag o UUID del objeto (valor inicial)
    core::Vec3 v{};
    std::uint32_t entity = kNoEntity;  // entity en Play: el objeto ya resuelto

    // "true", "5", "2.5", "hola", "Jugador", "1 2 3" con el tipo dado.
    static Value parse(VarType type, const std::string& text);
    std::string text() const;
    double number() const;  // bool 0/1, int, float; el resto 0
    bool truthy() const;    // bool, numero != 0, texto no vacio, entity puesta
};

struct Variable {
    std::string name;
    Value value;
};

// --- Asset ---------------------------------------------------------------------

enum class ConditionKind : int { Variable = 0, Trigger = 1, Timer = 2, Lua = 3 };
inline constexpr int kConditionKindCount = 4;

enum class Compare : int {
    Equal = 0,
    NotEqual = 1,
    Greater = 2,
    Less = 3,
    GreaterEqual = 4,
    LessEqual = 5,
    IsTrue = 6,   // bool / numero / texto / entity "con valor"
    IsFalse = 7,
};
inline constexpr int kCompareCount = 8;
const char* compareKey(Compare c);    // "==", "!=", ">", "<", ">=", "<=", "true", "false"
const char* compareLabel(Compare c);  // "igual a", "mayor que"...
Compare compareFromKey(const std::string& key, Compare fallback = Compare::Equal);

struct Condition {
    ConditionKind kind = ConditionKind::Variable;
    std::string variable;   // Variable: la de la pizarra; Trigger: su nombre
    Compare compare = Compare::Greater;
    std::string value;      // con el tipo de la variable; "$otra" = el valor de otra variable
    float seconds = 1.0f;   // Timer: segundos en el estado de origen
    std::string expression; // Lua: expresion booleana (ve las variables por su nombre, entity, sm, self)
};

// Origen de una transicion desde "Cualquier estado".
inline constexpr int kAnyState = -1;

struct Transition {
    int from = 0;  // indice de estado o kAnyState
    int to = 0;
    int priority = 0;            // mayor = se mira antes
    bool allow_self = false;     // Cualquier estado: tambien si ya esta en `to`
    std::vector<Condition> conditions;  // todas; sin condiciones = enseguida
};

struct State {
    std::string name = "Estado";
    core::Vec2 position{};
    core::Vec3 color{0.30f, 0.30f, 0.33f};
    std::string code;    // Lua del estado
    std::string script;  // o un .lua de Assets (manda si no esta vacio)
};

struct StateMachineAsset {
    Uuid uuid{};
    std::vector<Variable> variables;
    std::vector<State> states;
    std::vector<Transition> transitions;
    int entry_state = 0;
    std::string any_code;  // codigo de "Cualquier estado" (corre siempre)
    core::Vec2 entry_position{-280.0f, 0.0f};
    core::Vec2 any_state_position{-280.0f, 140.0f};

    int findState(const std::string& name) const;  // -1 si no
    const Variable* findVariable(const std::string& name) const;
    Variable* findVariable(const std::string& name);
    // Quita un estado y arregla los indices de transiciones y la entrada.
    void removeState(int index);
    // Renombra una variable tambien en las condiciones que la usan.
    void renameVariable(const std::string& from, const std::string& to);
};

// JSON del .crfsm. En el texto, "from"/"to" de una transicion pueden ser el
// indice o el nombre del estado ("any" = Cualquier estado) y "entry" el
// nombre o el indice: asi lo puede escribir una persona o una IA.
bool stateMachineFromJson(const std::string& text, StateMachineAsset& out, std::string* error = nullptr);
std::string stateMachineToJson(const StateMachineAsset& machine);
bool loadStateMachine(const std::filesystem::path& path, StateMachineAsset& out, std::string* error = nullptr);
// Sin UUID se guarda con uno nuevo (quien necesite saberlo lo pone antes).
bool saveStateMachine(const StateMachineAsset& machine, const std::filesystem::path& path,
                      std::string* error = nullptr);

// Problemas que impiden o estropean la ejecucion (estado de entrada fuera de
// rango, transicion a un estado que no existe, variable que no existe...).
std::vector<std::string> validateStateMachine(const StateMachineAsset& machine);

// Maquina de ejemplo para enemigos: Patrullar -> Perseguir -> Atacar, Huir
// con poca vida y Volver a casa (NavAgent + objeto "Jugador").
StateMachineAsset exampleEnemyStateMachine();

// --- Ejecucion -----------------------------------------------------------------

// Estado de la maquina en un objeto (lo guarda el componente; no se
// serializa). El editor lo lee para la depuracion en vivo.
struct Runtime {
    bool started = false;
    bool running = true;          // sm:stop() / sm:start()
    int state = -1;
    int previous = -1;
    float state_time = 0.0f;      // segundos en el estado actual
    float time = 0.0f;            // segundos desde que empezo
    int last_transition = -1;     // la ultima que se tomo (-1 = entrada, -2 = por codigo)
    float last_change_time = -100.0f;  // `time` del ultimo cambio
    std::uint64_t changes = 0;
    std::vector<Variable> vars;          // pizarra actual
    std::vector<std::string> triggers;   // pendientes (se consumen al mirar las transiciones)
    int requested = -1;                  // sm:go pendiente
    std::vector<std::string> history;    // ultimos cambios ("Patrullar -> Perseguir"), para el editor

    Value* find(const std::string& name);
    const Value* find(const std::string& name) const;
};

// Valor inicial de cada objeto: el del Inspector (por nombre) o el del asset.
struct VariableOverride {
    std::string name;
    int type = static_cast<int>(VarType::Float);
    std::string value;
};

// Pizarra nueva con los valores iniciales (las entity quedan sin resolver:
// las resuelve quien tenga el mundo, por su texto).
void resetRuntime(const StateMachineAsset& machine, Runtime& runtime,
                  const std::vector<VariableOverride>& overrides = {});

// Expresion Lua de una condicion: la evalua quien tenga Lua. (transicion,
// condicion, expresion) -> se cumple.
using LuaConditionFn = std::function<bool(int transition, int condition, const std::string& expression)>;

bool conditionHolds(const StateMachineAsset& machine, const Runtime& runtime, const Condition& condition,
                    int transition, int index, const LuaConditionFn& lua);

// La transicion que se cumple ahora (-1 si ninguna) entre las de Cualquier
// estado y las del estado actual: prioridad mayor primero; a igual
// prioridad, las de Cualquier estado y despues el orden de la lista.
int pickTransition(const StateMachineAsset& machine, const Runtime& runtime, const LuaConditionFn& lua);

struct StepResult {
    bool changed = false;
    int from = -1;
    int to = -1;
    int transition = -1;  // -2 = por codigo (sm:go)
};

// Entra en el estado de entrada (la primera vez).
StepResult startStateMachine(const StateMachineAsset& machine, Runtime& runtime);
// Un paso: suma el tiempo, aplica sm:go pendiente o la primera transicion que
// se cumpla y consume los triggers pendientes.
StepResult stepStateMachine(const StateMachineAsset& machine, Runtime& runtime, float dt,
                            const LuaConditionFn& lua = {});
// Aplica un sm:go pendiente (si lo hay) sin avanzar el tiempo.
StepResult applyRequestedState(const StateMachineAsset& machine, Runtime& runtime);
// Lo que hace un cambio (para los pasos de arriba y el editor): estado,
// tiempos e historial.
void changeState(const StateMachineAsset& machine, Runtime& runtime, int to, int transition);

// --- Componente ------------------------------------------------------------------

struct StateMachine {
    assets::AssetRef machine{{}, assets::AssetType::StateMachine};
    bool start_active = true;     // si no, espera a sm:start()
    bool debug = false;           // cada cambio de estado a la Consola
    std::vector<VariableOverride> variables;  // valores iniciales de este objeto
    Runtime runtime;              // en Play (no se guarda)

    void reflect(ecs::PropertyVisitor& v);
};

void registerStateMachineComponents();

}  // namespace cramion::ai

#endif  // CRAMION_CORE_AI_STATE_MACHINE_H
