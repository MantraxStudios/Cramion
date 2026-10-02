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
// Ejecucion: el grafo se compila a Lua (compileGraph) y corre en el mismo
// estado que los scripts, asi que llega a toda la API del motor. El
// componente VisualScript lo pone en un objeto: en Play (editor) y en el
// juego exportado cada objeto tiene su instancia con sus variables. Los
// nodos latentes (Delay) usan corrutinas. Para depurar, cada nodo que se
// ejecuta deja su hora y los pines su ultimo valor (el editor los ilumina y
// los ensena al pasar el raton), y los puntos de ruptura pausan el Play.

#include "CramionCore/Uuid.h"
#include "CramionCore/scripting/Scripting.h"

#include <CramionFX/core/Math.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cramion::ecs {
class PropertyVisitor;
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
};

struct Node {
    int id = 0;
    std::string kind;     // "event.start", "flow.branch", "call", "var.get"...
    core::Vec2 position{};
    std::string fn;       // call: "Audio.playOneShot" / "Entity:translate" / "Entity.position"; var.*: la variable
    bool pure = false;    // call: sin pines de ejecucion (se calcula al usarse)
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    std::string comment;  // texto de un nodo comentario (o nota de cualquier nodo)
    core::Vec2 size{};    // comentario: tamano de la caja
    bool breakpoint = false;  // punto de ruptura (solo en el editor; tambien se guarda)

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

struct Graph {
    Uuid uuid{};
    std::vector<Variable> variables;
    std::vector<Node> nodes;
    std::vector<Link> links;
    int next_id = 1;

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
};

// --- Catalogo de nodos ---------------------------------------------------------
struct NodeInfo {
    std::string kind;
    std::string title;
    std::string category;     // "Eventos", "Flujo", "Variables", "Matematicas"...
    std::string description;
    std::string keywords;     // para la busqueda ("if si condicion")
    bool event = false;       // sin entrada exec; empieza una cadena
    bool pure = false;        // sin pines exec
    bool latent = false;      // espera (Delay): usa corrutinas
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

// --- Compilar ------------------------------------------------------------------
struct NodeError {
    int node = 0;  // 0 = del grafo entero
    std::string message;
};

struct CompileResult {
    bool ok = false;
    std::string lua;                    // clase Lua (como un script: return G)
    std::vector<NodeError> errors;
    std::vector<int> line_nodes;        // line_nodes[linea - 1] = nodo de esa linea del Lua (0 = ninguno)
    int nodeAtLine(int line) const;
};
// `chunk`: la ruta relativa del .crgraph (sale en los errores de Lua).
CompileResult compileGraph(const Graph& graph, const std::string& chunk);

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

void registerVisualScriptComponents();

}  // namespace cramion::vscript

#endif  // CRAMION_CORE_SCRIPTING_VISUAL_SCRIPT_H
