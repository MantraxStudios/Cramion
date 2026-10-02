#ifndef CRAMION_CORE_AI_BEHAVIOR_TREE_H
#define CRAMION_CORE_AI_BEHAVIOR_TREE_H

// Arboles de comportamiento (Behavior Trees) para la IA, como los de Unreal:
// una raiz, nodos compuestos que deciden el orden, tareas en las hojas,
// decoradores (condiciones y modificadores) y servicios (sensores que corren
// cada cierto tiempo mientras su rama esta activa) sobre una pizarra
// (Blackboard) de variables con tipo.
//
// Asset .crbt (JSON con UUID):
//   blackboard   claves bool / int / float / string / entity / vec3 con su
//                valor inicial (cada objeto puede cambiarlo en el Inspector)
//   nodes        nodes[0] es la Raiz (un solo hijo). Cada nodo: tipo, nombre,
//                posicion en el grafo, hijos (en orden de prioridad: el de la
//                izquierda primero), decoradores, servicios y sus parametros.
//
// Compuestos   Selector (el primer hijo que salga bien), Sequence (todos en
//              orden hasta que uno falle), Parallel (todos a la vez) y
//              Random Selector (en orden aleatorio).
// Tareas       Move To (NavMesh), Wait, Play Animation, Set Animator Param,
//              Rotate To Face, Set Blackboard, Find Random Point, Run Script
//              (Lua o C++), Send Message y Log.
// Decoradores  Blackboard (comparar una clave), Script Condition, Is At
//              Location, Cooldown, Loop, Time Limit, Inverter, Force Success,
//              Force Failure. Los de condicion pueden abortar ("observer
//              aborts"): Self (su rama, si deja de cumplirse) y Lower Priority
//              (las ramas de la derecha, si empieza a cumplirse).
// Servicios    Find Nearest With Tag, Distance To, Sight (cono de vision con
//              rayo de linea de vista), Hearing (ruidos y cercania) y Run
//              Script.
//
// En Play cada objeto con el componente BehaviorTree ejecuta su arbol (lo
// lleva ScriptSystem, despues de los Update). Desde Lua:
// entity:getBehaviorTree() -> bt:get/set, bt:start/stop/restart,
// bt:finishTask(true), BehaviorTree.registerTask("Nombre", fn),
// BehaviorTree.reportNoise(posicion, radio, quien) (ver Scripting.h).

#include "CramionCore/Uuid.h"
#include "CramionCore/ai/StateMachine.h"
#include "CramionCore/asset/AssetTypes.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace cramion::ecs {
class PropertyVisitor;
class World;
}  // namespace cramion::ecs

namespace cramion::navigation {
class NavigationSystem;
}

namespace cramion::physics {
class PhysicsSystem;
}

namespace cramion::ai {

inline constexpr const char* kBehaviorTreeExtension = ".crbt";

// --- Tipos de nodo -----------------------------------------------------------------

enum class BtNodeKind : int {
    Root = 0,
    // Compuestos
    Selector = 1,
    Sequence = 2,
    Parallel = 3,
    RandomSelector = 4,
    // Tareas
    MoveTo = 10,
    Wait = 11,
    PlayAnimation = 12,
    SetAnimatorParam = 13,
    RotateToFace = 14,
    SetBlackboard = 15,
    FindRandomPoint = 16,
    RunScript = 17,
    SendMessage = 18,
    Log = 19,
};

enum class BtDecoratorKind : int {
    Blackboard = 0,
    ScriptCondition = 1,
    IsAtLocation = 2,
    Cooldown = 3,
    Loop = 4,
    TimeLimit = 5,
    Inverter = 6,
    ForceSuccess = 7,
    ForceFailure = 8,
};
inline constexpr int kBtDecoratorKindCount = 9;

enum class BtServiceKind : int {
    FindNearestWithTag = 0,
    DistanceTo = 1,
    Sight = 2,
    Hearing = 3,
    RunScript = 4,
};
inline constexpr int kBtServiceKindCount = 5;

// Observer aborts (como en Unreal).
enum class BtAbort : int { None = 0, Self = 1, LowerPriority = 2, Both = 3 };
inline constexpr int kBtAbortCount = 4;

enum class BtStatus : int { Idle = 0, Running = 1, Success = 2, Failure = 3, Aborted = 4 };

// Todos los tipos de nodo (para menus y el JSON), en orden de menu.
const std::vector<BtNodeKind>& btNodeKinds();
const char* btNodeKey(BtNodeKind kind);    // "selector", "move_to"... (JSON)
const char* btNodeLabel(BtNodeKind kind);  // "Selector", "Move To"...
const char* btNodeTooltip(BtNodeKind kind);
BtNodeKind btNodeFromKey(const std::string& key, BtNodeKind fallback = BtNodeKind::Sequence);
bool btIsComposite(BtNodeKind kind);  // Selector, Sequence, Parallel, Random Selector
bool btIsTask(BtNodeKind kind);

const char* btDecoratorKey(BtDecoratorKind kind);
const char* btDecoratorLabel(BtDecoratorKind kind);
const char* btDecoratorTooltip(BtDecoratorKind kind);
BtDecoratorKind btDecoratorFromKey(const std::string& key, BtDecoratorKind fallback = BtDecoratorKind::Blackboard);
// Los que se evaluan para entrar (y pueden abortar): Blackboard, Script
// Condition, Is At Location y Cooldown.
bool btIsCondition(BtDecoratorKind kind);

const char* btServiceKey(BtServiceKind kind);
const char* btServiceLabel(BtServiceKind kind);
const char* btServiceTooltip(BtServiceKind kind);
BtServiceKind btServiceFromKey(const std::string& key, BtServiceKind fallback = BtServiceKind::DistanceTo);

const char* btAbortKey(BtAbort abort);    // "none", "self", "lower_priority", "both"
const char* btAbortLabel(BtAbort abort);  // "Ninguno", "Self"...
BtAbort btAbortFromKey(const std::string& key);

const char* btStatusLabel(BtStatus status);

// --- Parametros --------------------------------------------------------------------

// Los parametros de un nodo, decorador o servicio. Cada tipo usa algunos (ver
// btParamsOf): asi el editor y el JSON son genericos.
struct BtParams {
    std::string key;    // clave principal de la pizarra
    std::string key2;   // segunda clave (salida)
    std::string key3;   // tercera clave (salida)
    std::string text;   // tag, clip, parametro, funcion, mensaje, texto
    std::string value;  // valor (con el tipo de la clave; "$otra" = otra clave)
    float number = 0.0f;
    float number2 = 0.0f;
    float number3 = 0.0f;
    int option = 0;     // comparacion, politica, tipo...
    bool flag = false;
};

enum class BtField : int { Key, Key2, Key3, Text, Value, Number, Number2, Number3, Option, Flag };

// Descripcion de un parametro de un tipo: donde se guarda, nombre en el JSON,
// etiqueta y ayuda. Option usa `options` (nombres separados por '|').
struct BtParamInfo {
    BtField field = BtField::Key;
    const char* json = "";
    const char* label = "";
    const char* tooltip = "";
    float default_number = 0.0f;
    const char* options = nullptr;  // Option: "Uno|Dos|Tres"
    int key_types = -1;             // Key*: bits de VarType aceptados (-1 = cualquiera)
};

const std::vector<BtParamInfo>& btParamsOf(BtNodeKind kind);
const std::vector<BtParamInfo>& btParamsOf(BtDecoratorKind kind);
const std::vector<BtParamInfo>& btParamsOf(BtServiceKind kind);
// Parametros con sus valores por defecto para un tipo.
BtParams btDefaultParams(const std::vector<BtParamInfo>& infos);
// Lectura y escritura generica de un campo (el editor).
std::string* btTextField(BtParams& p, BtField field);
float* btNumberField(BtParams& p, BtField field);

// --- Asset ---------------------------------------------------------------------------

struct BtDecorator {
    BtDecoratorKind kind = BtDecoratorKind::Blackboard;
    BtAbort abort = BtAbort::None;
    BtParams params;
};

struct BtService {
    BtServiceKind kind = BtServiceKind::DistanceTo;
    float interval = 0.5f;   // segundos entre cada vez
    float deviation = 0.1f;  // +- al azar (para que no corran todos a la vez)
    BtParams params;
};

struct BtNode {
    BtNodeKind kind = BtNodeKind::Sequence;
    std::string name;      // titulo (vacio = el del tipo)
    std::string comment;
    core::Vec2 position{};
    std::vector<int> children;  // indices en nodes, de izquierda a derecha
    std::vector<BtDecorator> decorators;
    std::vector<BtService> services;
    BtParams params;

    std::string title() const;  // name o la etiqueta del tipo
};

struct BehaviorTreeAsset {
    Uuid uuid{};
    std::vector<Variable> blackboard;
    std::vector<BtNode> nodes;  // nodes[0] = Raiz

    // Con una Raiz si esta vacio.
    void ensureRoot();
    const Variable* findKey(const std::string& name) const;
    Variable* findKey(const std::string& name);
    // Padre de un nodo (-1 si es la raiz o esta suelto).
    int parentOf(int node) const;
    // Nuevo nodo (con sus parametros por defecto). parent >= 0: se engancha
    // al final de sus hijos. Devuelve el indice.
    int addNode(BtNodeKind kind, int parent = -1, core::Vec2 position = {});
    // Quita un nodo y todo lo que cuelga de el (la Raiz no). Arregla indices.
    void removeSubtree(int node);
    // Quita solo ese nodo; sus hijos quedan sueltos.
    void removeNode(int node);
    // Engancha `child` a `parent` (lo suelta de su padre anterior). false si
    // crearia un ciclo, si el padre no admite hijos o si es la Raiz.
    bool attach(int child, int parent, int index = -1);
    void detach(int child);
    // Ordena los hijos de cada nodo por su X (como Unreal: la izquierda manda).
    void sortChildrenByPosition();
    // Coloca el arbol de arriba a abajo (los sueltos, a la derecha).
    void autoLayout();
    // Renombra una clave tambien en los parametros que la usan.
    void renameKey(const std::string& from, const std::string& to);
    // Indices en orden de ejecucion (profundidad, izquierda primero) desde la raiz.
    std::vector<int> executionOrder() const;
};

bool behaviorTreeFromJson(const std::string& text, BehaviorTreeAsset& out, std::string* error = nullptr);
std::string behaviorTreeToJson(const BehaviorTreeAsset& tree);
bool loadBehaviorTree(const std::filesystem::path& path, BehaviorTreeAsset& out, std::string* error = nullptr);
bool saveBehaviorTree(const BehaviorTreeAsset& tree, const std::filesystem::path& path, std::string* error = nullptr);

// Problemas (claves que no existen, compuestos sin hijos, nodos sueltos...).
std::vector<std::string> validateBehaviorTree(const BehaviorTreeAsset& tree);

// Arbol de ejemplo para un guardia: ve al "Player" -> lo persigue y ataca; si
// lo pierde va al ultimo sitio donde lo vio; si no, patrulla puntos al azar.
BehaviorTreeAsset exampleGuardBehaviorTree();

// --- Ejecucion -----------------------------------------------------------------------

struct BtNodeState {
    BtStatus status = BtStatus::Idle;  // ultimo resultado
    bool active = false;               // en la rama que corre
    bool fresh = false;                // la tarea acaba de entrar
    int child = 0;                     // compuesto: posicion del hijo actual
    std::vector<int> order;            // Random Selector: orden de los hijos
    std::vector<BtStatus> done;        // Parallel: resultado de cada hijo
    float enter_time = 0.0f;
    float finish_time = -100.0f;       // para el editor (colores que se apagan)
    float wait = 0.0f;                 // Wait: segundos elegidos
    int loops = 0;
    std::vector<float> service_next;   // por servicio
    std::vector<float> cooldown_until; // por decorador
    bool moving = false;               // Move To: se pidio al NavAgent
    core::Vec3 move_target{};
    std::uint64_t runs = 0;            // veces que entro (editor)
};

// Un ruido (pasos, disparo...) que oye el servicio Hearing.
struct BtNoise {
    core::Vec3 position{};
    float radius = 10.0f;      // hasta donde se oye
    std::uint32_t instigator = kNoEntity;
    std::string tag;           // de quien es (opcional)
    float time = 0.0f;         // segundos de juego cuando sono
};

struct BtRuntime {
    bool started = false;
    bool running = true;
    float time = 0.0f;
    std::uint64_t ticks = 0;
    std::uint64_t cycles = 0;      // veces que la raiz termino
    std::vector<Variable> vars;    // pizarra actual
    std::vector<BtNodeState> nodes;
    std::vector<int> active_path;  // raiz .. tarea activa (para el editor)
    int active_task = -1;
    BtStatus pending_finish = BtStatus::Idle;  // bt:finishTask() para Run Script
    std::vector<std::string> history;          // ultimos eventos (abortos, fallos)
    std::mt19937 rng{1234u};

    Value* find(const std::string& name);
    const Value* find(const std::string& name) const;
};

// Lo que el arbol necesita del mundo. Sin navegacion, Move To mueve el
// Transform en linea recta; sin fisica, Sight no comprueba paredes.
struct BtContext {
    ecs::World* world = nullptr;
    std::uint32_t self = kNoEntity;  // entt::entity del objeto
    navigation::NavigationSystem* navigation = nullptr;
    physics::PhysicsSystem* physics = nullptr;
    const std::vector<BtNoise>* noises = nullptr;
    float now = 0.0f;  // reloj de los ruidos (BtNoise::time)
    float dt = 0.0f;
    bool debug = false;

    // Run Script (tarea): first = acaba de entrar. Devuelve Running, Success
    // o Failure (Idle = la funcion no existe).
    std::function<BtStatus(int node, const std::string& function, bool first)> script_task;
    std::function<void(int node, const std::string& function)> script_abort;
    std::function<bool(int node, int decorator, const std::string& expression)> script_condition;
    std::function<void(int node, int service, const std::string& function)> script_service;
    std::function<void(std::uint32_t target, const std::string& message, const std::string& value)> send_message;
    std::function<void(int level, const std::string& text)> log;
};

// Pizarra nueva con los valores iniciales y los del Inspector; las claves
// entity se resuelven por nombre, tag o UUID si hay mundo.
void resetBehaviorTree(const BehaviorTreeAsset& tree, BtRuntime& runtime,
                       const std::vector<VariableOverride>& overrides = {}, const ecs::World* world = nullptr);
// Un frame: aborts, servicios, la rama activa. Devuelve el estado de la raiz.
BtStatus tickBehaviorTree(const BehaviorTreeAsset& tree, BtRuntime& runtime, BtContext& context);
// Corta todo lo que corre (Move To se para, Run Script recibe el aborto).
void abortBehaviorTree(const BehaviorTreeAsset& tree, BtRuntime& runtime, BtContext& context);

// Comparacion de una clave de la pizarra (decorador Blackboard).
bool btCompare(const std::vector<Variable>& vars, const std::string& key, Compare compare, const std::string& value);
// Objeto de una clave entity (o kNoEntity) y posicion de una clave entity/vec3.
std::uint32_t btResolveEntity(const ecs::World& world, const std::string& text);
bool btKeyPosition(const ecs::World& world, const BtRuntime& runtime, const std::string& key, core::Vec3& out);

// --- Componente ------------------------------------------------------------------------

struct BehaviorTree {
    assets::AssetRef tree{{}, assets::AssetType::BehaviorTree};
    bool start_active = true;  // si no, espera a bt:start()
    bool debug = false;        // abortos y fallos a la Consola
    float tick_interval = 0.0f;  // 0 = cada frame; si no, segundos entre ticks
    std::vector<VariableOverride> variables;  // valores iniciales de este objeto
    BtRuntime runtime;         // en Play (no se guarda)
    float tick_accumulator = 0.0f;

    void reflect(ecs::PropertyVisitor& v);
};

void registerBehaviorTreeComponents();

}  // namespace cramion::ai

#endif  // CRAMION_CORE_AI_BEHAVIOR_TREE_H
