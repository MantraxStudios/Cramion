#ifndef CRAMION_CORE_SCRIPTING_VISUAL_SCRIPT_H
#define CRAMION_CORE_SCRIPTING_VISUAL_SCRIPT_H

// Visual Scripting (como los Blueprints de Unreal): grafos de nodos en vez
// de codigo.
//
// Asset .crgraph (JSON con UUID):
//   Variables  con tipo (bool, int, float, string, vec3, entity, any), valor
//              inicial y si sale en el Inspector (cada objeto la puede cambiar)
//   Nodos      eventos (Start, Update, choques, triggers, acciones de
//              entrada, teclas, eventos propios), flujo (Branch, Sequence,
//              For, While, Delay, Do Once, Flip Flop, Gate, temporizadores),
//              variables, matematicas, logica, vectores, texto, objetos
//              (transform, buscar, crear prefabs, destruir, campos de
//              componentes), depuracion y CUALQUIER funcion de la API de
//              scripting (nodo "call": "Audio.playOneShot", "Entity:translate")
//   Enlaces    de un pin de salida a uno de entrada. Los pines "exec" (blancos)
//              llevan el orden de ejecucion; los de datos se calculan al
//              usarse (perezosos), como en Unreal.
//
// Ejecucion: un interprete de C++ (scripting/native/VisualScriptRuntime.cpp)
// lee el grafo una vez (compileGraph lo valida) y lo ejecuta; los nodos de
// la API llaman a sus funciones por nombre, asi que llega a toda la API del
// motor. El componente VisualScript lo pone en un objeto: en Play (editor) y
// en el juego exportado cada objeto tiene su instancia con sus variables.
// Los nodos latentes (Delay) guardan por donde iban y siguen en otro frame.
// Para depurar, cada nodo que se ejecuta deja su hora y los pines su ultimo
// valor (el editor los ilumina y los ensena al pasar el raton), y los puntos
// de ruptura pausan el Play.
//
// Fase 2 (como Unreal):
//   Construction Script  nodos en el grafo "construction" que empiezan en
//                        "event.construction": el editor los ejecuta al
//                        colocar o cambiar el objeto (fuera de Play).
//   Funciones            subgrafos "fn:Nombre" con su entrada (fn.entry) y
//                        sus Return (fn.return); se llaman con fn.call (puras
//                        o con ejecucion). No pueden esperar (Delay, Timeline).
//   Macros               subgrafos "macro:Nombre" (macro.entry, macro.exit y
//                        macro.call) con varios pines exec de entrada y de
//                        salida; si pueden esperar (Delay).
//   Timeline             nodo flow.timeline con pistas de float o vector
//                        (claves con curva), Play / Reverse / Stop y las
//                        salidas Update y Finished.
// Todos los nodos estan en Graph::nodes; Node::graph dice en que grafo
// estan. Un .crgraph sin nada de esto se guarda igual que antes.

#include "CramionCore/Uuid.h"
#include "CramionCore/scripting/Scripting.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cramion::ecs {
class PropertyVisitor;
class Entity;
}

namespace cramion::vscript {

inline constexpr const char* kGraphExtension = ".crgraph";

// --- Tipos de pin ----------------------------------------------------------------
enum class PinType : int { Exec = 0, Any = 1, Bool = 2, Int = 3, Float = 4, String = 5, Vector = 6, Entity = 7 };
inline constexpr int kPinTypeCount = 8;
const char* pinTypeKey(PinType type);    // "exec", "any", "bool"...
const char* pinTypeLabel(PinType type);  // "Ejecucion", "Cualquiera", "Booleano"...
PinType pinTypeFromKey(const std::string& key, PinType fallback = PinType::Any);
// Se puede enlazar una salida de tipo `from` a una entrada de tipo `to`?
// (Int <-> Float, cualquier cosa -> Any / String, Any -> cualquier cosa.)
bool pinTypesCompatible(PinType from, PinType to);

struct Pin {
    std::string name;
    PinType type = PinType::Any;
    std::string value;  // valor por defecto de una entrada sin enlace ("5", "true", "hola", "1 2 3")
    std::vector<std::string> options;  // valores de un pin tipo enum (los pone el catalogo; no se guardan)
};

// --- Timeline (nodo flow.timeline) ---------------------------------------------
enum class CurveInterp : int { Linear = 0, Constant = 1, Smooth = 2 };

struct TimelineKey {
    float time = 0.0f;
    core::Vec3 value{};  // pista float: solo x
    CurveInterp interp = CurveInterp::Linear;  // hasta la clave siguiente
};

struct TimelineTrack {
    std::string name = "Alpha";
    PinType type = PinType::Float;  // Float o Vector
    std::vector<TimelineKey> keys;  // ordenadas por tiempo
};

struct Timeline {
    float length = 1.0f;   // segundos
    bool loop = false;
    bool autoplay = false;  // empieza solo al empezar Play
    std::vector<TimelineTrack> tracks;
};

// Valor de una pista en un tiempo (antes de la primera clave, la primera;
// despues de la ultima, la ultima; sin claves, 0).
core::Vec3 evaluateTrack(const TimelineTrack& track, float time);
// Salidas fijas de un Timeline antes de las pistas (Update, Finished, Direccion, Tiempo).
inline constexpr int kTimelineFixedOutputs = 4;

struct Node {
    int id = 0;
    std::string kind;     // "event.start", "flow.branch", "call", "var.get"...
    std::string graph;    // "" Event Graph, "construction", "fn:Nombre" o "macro:Nombre"
    core::Vec2 position{};
    std::string fn;       // call: "Audio.playOneShot" / "Entity:translate" / "Entity.position"; var.*: la variable
    bool pure = false;    // call: sin pines de ejecucion (se calcula al usarse)
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    std::string comment;  // texto de un nodo comentario (o nota de cualquier nodo)
    core::Vec2 size{};    // comentario: tamano de la caja
    bool breakpoint = false;  // punto de ruptura (solo en el editor; tambien se guarda)
    // Se guardan solo si no son el valor por defecto.
    bool disabled = false;            // desactivado: deja pasar la ejecucion (primera salida exec)
    std::uint32_t comment_color = 0;  // RGBA, 0 = defecto
    Timeline timeline;                // solo flow.timeline

    int input(const std::string& name) const;   // indice o -1
    int output(const std::string& name) const;
};

struct Link {
    int from_node = 0;
    int from_pin = 0;  // indice en outputs
    int to_node = 0;
    int to_pin = 0;    // indice en inputs
    friend bool operator==(const Link&, const Link&) = default;
};

struct Variable {
    std::string name;
    PinType type = PinType::Float;
    std::string value;     // valor inicial (texto con el tipo)
    bool exposed = true;   // sale en el Inspector del componente
    std::string tooltip;
};

// --- Funciones y macros ------------------------------------------------------------
inline constexpr const char* kConstructionGraph = "construction";

enum class SubgraphKind : int { Function = 0, Macro = 1 };

struct Subgraph {
    std::string name;
    SubgraphKind kind = SubgraphKind::Function;
    std::vector<Pin> inputs;   // parametros (macro: tambien pines exec)
    std::vector<Pin> outputs;  // resultados (macro: tambien pines exec)
    bool pure = false;         // funcion: sus llamadas no tienen pines exec
    std::string description;
    std::string key() const;   // Node::graph de sus nodos ("fn:Nombre")
};
std::string functionKey(const std::string& name);  // "fn:Nombre"
std::string macroKey(const std::string& name);     // "macro:Nombre"
// Nodos de los subgrafos: fn.entry, fn.return, fn.call, macro.entry, macro.exit, macro.call.
bool isSubgraphNode(const std::string& kind);

struct Graph {
    Uuid uuid{};
    std::vector<Variable> variables;
    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<Subgraph> functions;  // funciones y macros
    int next_id = 1;
    // Lo usa el editor: Node::graph de los nodos sin grafo que entran con add()
    // (el grafo que se esta viendo). No se guarda.
    std::string editing;

    Node* find(int id);
    const Node* find(int id) const;
    const Variable* findVariable(const std::string& name) const;
    Variable* findVariable(const std::string& name);
    // Enlace que llega a una entrada (o nullptr).
    const Link* linkTo(int node, int pin) const;
    // Quita un nodo y sus enlaces.
    void removeNode(int id);
    // Enlaza (una entrada de datos solo tiene un enlace; una salida exec
    // solo uno: se reemplazan). false si no se puede (tipos, mismo nodo...).
    bool connect(int from_node, int from_pin, int to_node, int to_pin, std::string* error = nullptr);
    // Renombra una variable tambien en sus nodos.
    void renameVariable(const std::string& from, const std::string& to);
    // Nodo nuevo con id (lo devuelve ya dentro del grafo).
    Node& add(Node node);

    // Funcion o macro por su key ("fn:X") o por nombre y tipo (nullptr si no hay).
    const Subgraph* findSubgraph(const std::string& key) const;
    Subgraph* findSubgraph(const std::string& key);
    const Subgraph* findFunction(const std::string& name, SubgraphKind kind) const;
    // Funcion (o macro) nueva con su nodo de entrada y su Return / salida.
    // El nombre se hace unico ("MiFuncion", "MiFuncion2"...).
    Subgraph& addSubgraph(const std::string& name, SubgraphKind kind);
    // Tras cambiar la firma: rehace los pines de su entrada, sus Return / salida
    // y sus llamadas (valores y enlaces por nombre de pin).
    void syncSubgraph(const std::string& key);
    // Renombra (sus nodos y sus llamadas tambien). false si el nombre esta usado.
    bool renameSubgraph(const std::string& key, const std::string& name);
    // Quita la funcion / macro con sus nodos y sus llamadas.
    void removeSubgraph(const std::string& key);
    // Cambia los pines de un nodo conservando valores y enlaces por nombre.
    void setPins(int id, std::vector<Pin> inputs, std::vector<Pin> outputs);
    // Timeline: rehace sus salidas (una por pista) tras cambiar sus pistas.
    void syncTimeline(int id);
};

// Pines de los nodos de una funcion / macro segun su firma.
void subgraphNodePins(const Subgraph& sub, const std::string& kind, std::vector<Pin>& inputs, std::vector<Pin>& outputs);
// Las salidas de un Timeline (las fijas y una por pista).
std::vector<Pin> timelineOutputs(const Timeline& timeline);

// --- Catalogo de nodos ---------------------------------------------------------
struct NodeInfo {
    std::string kind;
    std::string title;
    std::string category;     // "Eventos", "Flujo", "Variables", "Matematicas"...
    std::string description;
    std::string keywords;     // para la busqueda ("if si condicion")
    bool event = false;       // sin entrada exec; empieza una cadena
    bool pure = false;        // sin pines exec
    bool latent = false;      // espera (Delay): sigue en otro frame
    // Pines que el usuario puede anadir mas alla del catalogo (Switch, MultiGate,
    // Make Array, Format Text...): graphFromJson conserva los guardados, como Sequence.
    bool dynamic_inputs = false;
    bool dynamic_outputs = false;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
};
// Todos los nodos integrados (sin los de la API, que salen de ella).
const std::vector<NodeInfo>& nodeCatalog();
const NodeInfo* findNodeInfo(const std::string& kind);
// Nodo de un tipo integrado en una posicion (sin id: Graph::add le da uno).
Node makeNode(const std::string& kind, core::Vec2 position = {});
// Titulo que se ve en el nodo ("Branch", "Set vida", "Audio.playOneShot").
std::string nodeTitle(const Node& node);
// Color de la cabecera por categoria (r, g, b en 0..1).
core::Vec3 nodeColor(const Node& node);

// --- Objeto hijo por ruta (nodo "entity.child", Node::fn = la ruta) ------------
// Ruta de `e` desde `root`: nombres separados por '/' ("Cuerpo/Malla"); si
// hay hermanos con el mismo nombre, el 2.o en adelante lleva su numero
// ("Hueso[2]"). '/', '[', ']' y '\' dentro de un nombre van con '\'.
// Vacia si `e` es `root` o no esta debajo de el.
std::string entityChildPath(const ecs::Entity& root, const ecs::Entity& e);
// El objeto de esa ruta debajo de `root` (invalido si no existe).
ecs::Entity resolveEntityChildPath(const ecs::Entity& root, const std::string& path);
// Nombre del ultimo tramo ("Hueso [2]"), el que se ve en el nodo.
std::string entityChildPathName(const std::string& path);

// Nodo que llama a una funcion de la API ("Tabla.funcion", "Entity:metodo")
// o lee/escribe una propiedad ("Entity.position": `set` = escribir).
// `doc_args`: los argumentos tal como los documenta el autocompletado
// ("\"W\"", "Vec3, \"impulse\"", "desde, hasta"); de ahi salen los pines con
// su tipo y su valor de ejemplo.
Node makeCallNode(const std::string& callee, const std::string& doc_args, const std::string& description,
                  core::Vec2 position = {});
Node makePropertyNode(const std::string& callee, bool set, const std::string& description = {}, core::Vec2 position = {});
// Las funciones que solo leen (get..., is..., has..., find...) se crean puras.
bool looksPure(const std::string& callee);

// --- Aspecto y busqueda por contexto (editor) ----------------------------------
enum class NodeLook : std::uint8_t { Default, Event, Function, Pure, Flow, Compact, Variable, Reroute, Comment };
NodeLook nodeLook(const Node& node);
const char* nodeSymbol(const Node& node);   // Compact: "+", "-", "×", "÷", "==", "AND"...; "" si no

// Bit i = PinType i (Exec incluido) presente en entradas/salidas, para la busqueda por contexto.
std::uint32_t inputTypeMask(const Node& node);
std::uint32_t outputTypeMask(const Node& node);

// Conversion que el editor inserta cuando connect() falla solo por tipos: "conv.int_to_float"...
std::optional<std::string> conversionKind(PinType from, PinType to);

// "bool"/"booleano" -> Bool, "numero" -> Float, "texto" -> String, "Vec3" -> Vector,
// "Entity" -> Entity; "objeto", "valor", "Quat", "Mesh", handles, "lista de X" y
// texto libre -> Any; "X o nil" -> X; vacio -> Exec-sin-salida (Any).
PinType typeFromDocText(const std::string& returns);

struct ApiNodeTemplate {
    std::string category;     // "API: Entity"
    std::string title;        // "Entity:moveTo" / "Get Entity.position"
    std::string keywords;
    std::string description;
    Node node;                // pines tipados (Doc.args + Doc.returns); pure segun looksPure / getter
};
// Una por Function/Method, Get por Property y Set solo si es escribible (Entry::assign).
// Se construye una vez por NativeApi (cache por puntero). En el editor:
// scripting::ScriptSystem::apiRegistry().
const std::vector<ApiNodeTemplate>& apiNodeTemplates(const scripting::api::NativeApi& api);

// Format Text (str.format): los {nombre} del texto, en orden y sin repetir. El
// editor pone una entrada por cada uno (despues de "Formato").
std::vector<std::string> formatTextArguments(const std::string& format);

// --- Archivo -------------------------------------------------------------------
// En el texto, un enlace puede nombrar los pines ("out": "Verdadero") o dar
// su indice, y un nodo integrado puede traer solo {"id", "kind", "values":
// {"Condicion": "true"}}: los pines salen del catalogo. Asi lo puede
// escribir una persona o una IA.
bool graphFromJson(const std::string& text, Graph& out, std::string* error = nullptr);
std::string graphToJson(const Graph& graph);
bool loadGraph(const std::filesystem::path& path, Graph& out, std::string* error = nullptr);
bool saveGraph(const Graph& graph, const std::filesystem::path& path, std::string* error = nullptr);
// Grafo de ejemplo de un archivo nuevo: Start -> "Hola" y Update que gira.
Graph exampleGraph();

// --- Validar -------------------------------------------------------------------
struct NodeError {
    int node = 0;  // 0 = del grafo entero
    std::string message;
    bool warning = false;  // aviso: no impide ejecutar (p. ej. tipos de un nodo de la API)
};

struct CompileResult {
    bool ok = false;                // sin errores (los avisos no cuentan)
    std::vector<NodeError> errors;  // errores y avisos
};
// Comprueba el grafo (tipos de nodo, variables, enlaces, numeros, ciclos):
// el editor ensena los errores sobre sus nodos y un grafo con errores no se
// ejecuta. `chunk`: la ruta relativa del .crgraph (no hace falta).
CompileResult compileGraph(const Graph& graph, const std::string& chunk = {});

// Tipo real de un pin (los de Get/Set de una variable tienen el de ella).
PinType nodePinType(const Graph& graph, const Node& node, bool output, int index);
// Nodo sin pines de ejecucion: se calcula cuando alguien usa su valor.
bool isPureNode(const Node& node);

// El valor escrito en una entrada sin enlace (o en una variable) con el tipo
// del pin: "5", "true", "1 2 3", un texto o un objeto (vacio = este objeto;
// si no, el primero con ese nombre). `ok` = false si un numero no lo es.
struct Literal {
    enum class Kind : int { Nil, Bool, Number, Text, Vector, Self, Find };
    Kind kind = Kind::Nil;
    bool boolean = false;
    double number = 0.0;
    std::string text;  // Text; Find: el nombre del objeto
    core::Vec3 vector{};
};
Literal parseLiteral(const std::string& text, PinType type, bool* ok = nullptr);

// Variables que salen en el Inspector, como propiedades de script.
std::vector<scripting::ScriptProperty> exposedProperties(const Graph& graph);
scripting::PropertyType propertyTypeOf(PinType type);

// --- Componente ------------------------------------------------------------------
struct VisualScript {
    std::string graph;   // ruta del .crgraph dentro de Assets
    bool enabled = true;
    std::vector<scripting::ScriptProperty> properties;  // valores de este objeto (por nombre)

    void reflect(ecs::PropertyVisitor& v);
};

// Objeto que creo el Construction Script de `owner` (Spawn): se borra y se
// vuelve a crear cada vez que el script corre, como los componentes que
// anade el Construction Script de Unreal. No sale en "Add Component".
struct ConstructionOwned {
    Uuid owner{};

    void reflect(ecs::PropertyVisitor& v);
};

void registerVisualScriptComponents();

}  // namespace cramion::vscript

#endif  // CRAMION_CORE_SCRIPTING_VISUAL_SCRIPT_H
