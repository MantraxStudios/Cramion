#ifndef CRAMION_CORE_ASSET_SHADER_GRAPH_H
#define CRAMION_CORE_ASSET_SHADER_GRAPH_H

// Shader Graph (.crshadergraph): un shader de superficie hecho con nodos,
// como el Shader Graph de Unity o el editor de materiales de Unreal. El grafo
// (JSON) se traduce a un .crshader normal (ver SurfaceShader.h) junto a el
// (Rocas.crshadergraph -> Rocas.crshader), asi lo usa el mismo camino de
// siempre: material, recompilacion en caliente, export y Android.
//
// - Los nodos "Propiedad" (float, range, color, vector, textura) son las
//   lineas `property` del .crshader: salen en el Inspector del material.
// - El nodo Salida (Master) escribe la superficie: color base, alfa, metal,
//   rugosidad, oclusion, emision, normal (tangente o mundo) y, en los
//   vertices, un desplazamiento y la normal. Lo que no se conecta conserva
//   el valor del material (sus texturas y factores).
// - Hay ademas un evaluador en CPU (vista previa del editor y pruebas) que
//   calcula el grafo igual que el GLSL generado.

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::assets {

inline constexpr const char* kShaderGraphExtension = ".crshadergraph";

namespace shadergraph {

// Tipo de un pin. Dynamic: acepta float/vec2/vec3/vec4 (el nodo trabaja con
// el mayor de sus entradas dinamicas, como el "Dynamic Vector" de Unity).
enum class PinType { Float, Vec2, Vec3, Vec4, Texture, Dynamic };

// Valor por defecto de una entrada sin conectar que no es una constante.
enum class PinDefault { Value, UV, Position, Normal, VertexNormal, ViewDirection, Time };

// Componentes de un tipo (Float 1 ... Vec4 4, Texture 0, Dynamic -1).
int pinWidth(PinType type);

struct PinDef {
    std::string name;
    PinType type = PinType::Float;
    core::Vec4 value{0.0f, 0.0f, 0.0f, 0.0f};
    PinDefault binding = PinDefault::Value;
};

// Que se edita del nodo en el panel de detalles.
enum class NodeField : std::uint32_t {
    None = 0,
    Name = 1u << 0,       // nombre de la propiedad / mascara / expresion
    Value = 1u << 1,      // valor (constante o por defecto de la propiedad)
    Range = 1u << 2,      // min / max
    Texture = 1u << 3,    // textura de vista previa (y por defecto al crear un material)
    Width = 1u << 4,      // componentes de la salida (nodo de codigo)
};
inline NodeField operator|(NodeField a, NodeField b) {
    return static_cast<NodeField>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
inline bool hasField(NodeField set, NodeField f) {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(f)) != 0;
}

struct NodeDef {
    std::string type;         // id en el JSON ("multiply")
    std::string title;        // "Multiplicar"
    std::string category;     // "Matematicas"
    std::string description;  // ayuda (tooltip y buscador)
    std::string keywords;     // nombres en ingles para el buscador ("multiply mul")
    std::vector<PinDef> inputs;
    std::vector<PinDef> outputs;
    NodeField fields = NodeField::None;
    int value_components = 0;  // 1..4 componentes de `value` (si tiene Value)
    bool color_value = false;  // `value` es un color (selector de color)
    bool fragment_only = false;  // no vale para los vertices
    bool unique = false;         // solo puede haber uno (Salida)
};

const std::vector<NodeDef>& nodeDefinitions();
const NodeDef* findNodeDef(const std::string& type);
// Pin por nombre (sin mayusculas ni tildes: "emision" = "Emisión") o por
// indice ("2"). -1 si no existe.
int findPin(const NodeDef& def, const std::string& name, bool output);

struct Node {
    int id = 0;
    std::string type;
    float x = 0.0f;
    float y = 0.0f;
    std::vector<core::Vec4> inputs;  // valores de las entradas sin conectar (por indice)
    std::string name;                // propiedad / mascara del swizzle / expresion
    core::Vec4 value{0.0f, 0.0f, 0.0f, 1.0f};
    float min = 0.0f;
    float max = 1.0f;
    std::string texture;  // textura de vista previa (ruta en Assets)
    bool preview = true;  // vista previa dentro del nodo
};

struct Link {
    int from_node = 0;
    int from_pin = 0;  // salida
    int to_node = 0;
    int to_pin = 0;    // entrada
};

struct Group {
    std::string title = "Grupo";
    float x = 0.0f, y = 0.0f, w = 300.0f, h = 200.0f;
    core::Vec4 color{0.35f, 0.45f, 0.65f, 1.0f};
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<Group> groups;
    int next_id = 1;
    std::string description;

    Node* find(int id);
    const Node* find(int id) const;
    const Node* master() const;
    // Crea un nodo del tipo con sus valores por defecto (no lo pone si el
    // tipo no existe o es unico y ya esta). Devuelve su id (0 si no).
    int add(const std::string& type, float x, float y);
    void remove(int id);  // con sus enlaces
    // Conecta (reemplaza el enlace que ya tuviera esa entrada). false si los
    // pines no existen, crearia un ciclo o los tipos no casan.
    bool connect(int from_node, int from_pin, int to_node, int to_pin, std::string* error = nullptr);
    void disconnectInput(int node, int pin);
    const Link* linkTo(int node, int pin) const;
    // Rellena las entradas que falten (grafo viejo o editado a mano).
    void normalize();
};

// Se pueden conectar una salida de tipo `from` a una entrada de tipo `to`.
bool compatible(PinType from, PinType to);

// Grafo nuevo: Salida + un color y una textura de ejemplo conectados.
Graph makeDefault();

bool parse(const std::string& text, Graph& out, std::string* error = nullptr);
std::string serialize(const Graph& graph);
bool load(const std::filesystem::path& file, Graph& out, std::string* error = nullptr);
bool save(const Graph& graph, const std::filesystem::path& file);

// --- Generacion del .crshader -----------------------------------------------
struct GenerateResult {
    std::string code;                 // el .crshader completo
    std::vector<std::string> errors;  // vacio si fue bien
    int error_node = 0;               // nodo del primer error (0 = ninguno)
    std::vector<int> line_nodes;      // por linea del codigo (1..n): el nodo que la genero (0 = ninguno)
    bool uses_time = false;           // la vista previa se anima
    bool has_vertex = false;
};

// `name`: el nombre del grafo (sale en el comentario de cabecera).
bool generate(const Graph& graph, const std::string& name, GenerateResult& out);

// El .crshader que se genera junto al grafo (misma carpeta y nombre).
std::filesystem::path generatedShaderPath(const std::filesystem::path& graph_file);
// El .crshadergraph del que sale un .crshader (vacio si no lo hay).
std::filesystem::path graphForShader(const std::filesystem::path& shader_file);
// El texto es un .crshader generado por un grafo (lleva la marca).
bool isGeneratedShader(const std::string& text);
// Genera y escribe el .crshader. No pisa un .crshader escrito a mano.
bool writeGeneratedShader(const Graph& graph, const std::filesystem::path& graph_file, GenerateResult& result,
                          std::string* error = nullptr);

// Nodo y mensaje de un error de compilacion ("Nombre.crshader:12: ...").
int nodeForErrorLine(const GenerateResult& result, const std::string& compile_error);

// --- Evaluacion en CPU (vistas previas) --------------------------------------
struct PreviewPoint {
    core::Vec2 uv;
    core::Vec3 position;  // en el mundo
    core::Vec3 normal;
    core::Vec3 view;      // del punto hacia la camara
    float time = 0.0f;
};

// Superficie resultante (lo que el Master escribe sobre la base).
struct PreviewSurface {
    core::Vec3 albedo{0.8f, 0.8f, 0.8f};
    float alpha = 1.0f;
    float metallic = 0.0f;
    float roughness = 0.5f;
    float occlusion = 1.0f;
    core::Vec3 emission{0.0f, 0.0f, 0.0f};
    core::Vec3 normal{0.0f, 0.0f, 1.0f};
};

// Lee una textura en la vista previa: `texture` = ruta en Assets (vacio = la
// textura principal del material). Devuelve RGBA 0..1.
using TextureSampler = std::function<core::Vec4(const std::string& texture, core::Vec2 uv)>;

class Evaluator {
public:
    struct Value {
        float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        int width = 1;        // 1..4 (0 = textura)
        std::string texture;  // textura (ruta en Assets; vacio = la del material)
    };

    Evaluator(const Graph& graph, TextureSampler sampler);
    // Valor de una salida (vec4; `width` recibe sus componentes, 0 = textura).
    core::Vec4 output(const PreviewPoint& point, int node, int pin, int* width = nullptr);
    // La superficie entera en ese punto (base gris si no se conecta).
    PreviewSurface surface(const PreviewPoint& point);

private:
    const std::vector<Value>& outputs(int node, int depth);
    Value input(const Node& node, int pin, int depth);

    const Graph& graph_;
    TextureSampler sampler_;
    PreviewPoint point_{};
    std::unordered_map<int, std::vector<Value>> memo_;  // salidas por nodo (en el punto actual)
};

}  // namespace shadergraph
}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_SHADER_GRAPH_H
