#ifndef CRAMION_CORE_MODELING_EDITABLE_MESH_H
#define CRAMION_CORE_MODELING_EDITABLE_MESH_H

// Modelado poligonal en el editor (como ProBuilder de Unity, con mas
// herramientas): una malla de poligonos (caras de N lados) que se guarda en la
// escena y se edita por vertices, aristas y caras.
//
//   PolyMesh          posiciones compartidas + caras (indices antihorarios
//                     vistos desde fuera). Cada cara lleva su hueco de
//                     material, su grupo de suavizado y su proyeccion de UV.
//   EditableMesh      el componente ("Malla editable"): guarda la PolyMesh y
//                     genera la malla del MeshRenderer de su entidad (y con
//                     ella la del MeshCollider). Se rehace al cambiar.
//   shapes::*         formas parametricas (Shapes.cpp)
//   ops::*            operaciones de topologia (MeshOps.cpp): extruir,
//                     inset, bisel, subdividir, bucles de aristas, puentes,
//                     soldar, booleanas, Catmull-Clark... (Csg.cpp: booleanas)
//
// Las operaciones trabajan en el espacio local de la entidad y mantienen la
// malla cerrada (las caras vecinas reciben los vertices nuevos de sus
// aristas). Las UV se calculan al generar la malla (proyeccion por cara: caja,
// plana o manual), asi que mover vertices no estira las texturas.

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cramion::ecs {
class Mesh;
class World;
class Entity;
}  // namespace cramion::ecs

namespace cramion::modeling {

using core::Mat4;
using core::Vec2;
using core::Vec3;

// --- UV por cara ---------------------------------------------------------------

enum class UvMode : int {
    Box = 0,     // proyecta en el plano del eje dominante de la normal (sin costuras al hacer bloques)
    Planar = 1,  // en el propio plano de la cara (rampas, techos inclinados)
    Manual = 2,  // las UV guardadas en la cara (Face::uv)
};
enum class UvFill : int {
    Tile = 0,     // metros: la textura se repite (escala = metros por repeticion)
    Fit = 1,      // cabe entera en 0..1 sin deformar
    Stretch = 2,  // estirada a 0..1 en cada eje
};

struct FaceUv {
    UvMode mode = UvMode::Box;
    UvFill fill = UvFill::Tile;
    Vec2 scale{1.0f, 1.0f};
    Vec2 offset{0.0f, 0.0f};
    float rotation = 0.0f;  // grados
    bool flip_u = false;
    bool flip_v = false;
    bool swap_uv = false;
    bool world_space = false;  // proyecta con la posicion en el mundo (las piezas encajan entre objetos)
};

struct Face {
    std::vector<std::uint32_t> v;  // indices en PolyMesh::positions (antihorario visto desde fuera)
    std::vector<Vec2> uv;          // por esquina; solo con UvMode::Manual
    int material = 0;              // hueco de material (submalla)
    int smoothing = 0;             // 0 = arista dura; mismo grupo > 0 = suaves entre si
    FaceUv uvs{};
};

// Arista sin direccion (a < b).
struct Edge {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    Edge() = default;
    Edge(std::uint32_t x, std::uint32_t y) : a(x < y ? x : y), b(x < y ? y : x) {}
    bool operator==(const Edge& o) const { return a == o.a && b == o.b; }
    bool operator<(const Edge& o) const { return a != o.a ? a < o.a : b < o.b; }
    std::uint64_t key() const { return (static_cast<std::uint64_t>(a) << 32) | b; }
};

class PolyMesh {
public:
    std::vector<Vec3> positions;
    std::vector<Face> faces;

    bool empty() const { return faces.empty(); }
    void clear();
    int addVertex(const Vec3& p);
    int addFace(std::vector<std::uint32_t> indices, int material = 0);

    // Normal (Newell, vale para caras no planas), centro, area.
    Vec3 faceNormal(int face) const;
    Vec3 faceCenter(int face) const;
    float faceArea(int face) const;
    // Todas las aristas, sin repetir (en orden de aparicion).
    std::vector<Edge> edges() const;
    // Caras de cada arista (1 = borde, 2 = interior; mas = no manifold).
    std::vector<int> facesOfEdge(const Edge& e) const;
    // Aristas de borde (con una sola cara).
    std::vector<Edge> borderEdges() const;
    // Caras que usan cada vertice.
    std::vector<std::vector<int>> vertexFaces() const;
    // Limites (minimo y maximo).
    void bounds(Vec3& min, Vec3& max) const;
    // Quita los vertices que no usa ninguna cara (reindexa).
    void removeUnusedVertices();
    // Quita caras con menos de 3 vertices distintos y vertices repetidos seguidos.
    void cleanup();
    // Vacio si esta bien; si no, el motivo.
    std::string validate() const;

    // Triangulos de una cara (indices en face.v, de 3 en 3), por recorte de
    // orejas en el plano de la cara (vale para concavas).
    std::vector<std::uint32_t> triangulateFace(int face) const;

    // Texto compacto (se guarda en la escena) y de vuelta.
    std::string serialize() const;
    bool deserialize(const std::string& text);
};

// --- Generar la malla del renderizador ---------------------------------------

struct SlotMaterial {
    Vec3 color{0.82f, 0.82f, 0.82f};
    float metallic = 0.0f;
    float roughness = 0.65f;
};

struct BuildOptions {
    float auto_smooth_angle = 0.0f;  // grados; > 0: suaviza tambien entre caras con menos angulo
    float uv_scale = 1.0f;           // multiplica la escala de UV de todas las caras
    Mat4 world = Mat4::identity();   // para las caras con UV en el mundo
    std::vector<SlotMaterial> slots;  // color por hueco (si no hay .crmat)
};

// Un vertice por esquina de cara; una submalla por hueco de material.
std::shared_ptr<ecs::Mesh> buildMesh(const PolyMesh& mesh, const BuildOptions& options);
// UV de una esquina con la proyeccion de su cara (las que se veran).
Vec2 faceCornerUv(const PolyMesh& mesh, int face, int corner, const Mat4& world, float uv_scale = 1.0f);

// --- Componente ------------------------------------------------------------------

struct EditableMesh {
    PolyMesh mesh;
    float auto_smooth_angle = 0.0f;
    float uv_scale = 1.0f;
    std::vector<SlotMaterial> slots{SlotMaterial{}};
    // Sube con cada cambio (no se guarda): la malla del renderizador se rehace.
    std::uint64_t revision = 1;

    void markModified() { ++revision; }
    void reflect(ecs::PropertyVisitor& v);
};

void registerModelingComponents();

// Lleva cada EditableMesh a la malla de su MeshRenderer (lo anade si falta).
// Barato si nada cambio; lo llaman RenderSync y la fisica cada frame.
void updateEditableMeshes(ecs::World& world);

// Una entidad nueva con la malla (MeshRenderer y, si `collider`, MeshCollider).
ecs::Entity createEditableEntity(ecs::World& world, PolyMesh mesh, const std::string& name, ecs::Entity parent,
                                 bool collider = true);

// --- Formas ------------------------------------------------------------------------

namespace shapes {
enum class Kind : int {
    Cube = 0, Plane, Cylinder, Cone, Sphere, Icosphere, Torus, Pipe, Prism, Wedge, Stairs, CurvedStairs,
    Arch, Door, Room, Capsule, Count
};
const char* kindName(Kind kind);      // en espanol, para la interfaz
const char* kindKey(Kind kind);       // "cube", "stairs"... (MCP, Lua)
bool kindFromKey(const std::string& key, Kind& kind);

struct Params {
    Vec3 size{1.0f, 1.0f, 1.0f};  // ancho (X), alto (Y), fondo (Z)
    int segments = 16;            // lados de cilindros, conos, esferas, arcos...
    int rings = 8;                // esfera: anillos; toro: tubo; capsula: medio anillo
    int subdivisions_x = 1;       // cubo/plano: cortes por eje
    int subdivisions_y = 1;
    int subdivisions_z = 1;
    int steps = 8;                // escaleras
    float thickness = 0.25f;      // tubo, arco, puerta, habitacion (pared)
    float inner_radius = 0.5f;    // escalera curva (radio interior), toro (radio del tubo)
    float angle = 180.0f;         // arco / escalera curva (grados)
    bool smooth = true;           // cilindros, esferas: lados suaves
    bool caps = true;             // tapas de cilindros y tubos
    bool sides = true;            // escaleras: laterales hasta el suelo
    Vec2 door{0.5f, 0.7f};        // puerta: hueco (fraccion del ancho y del alto)
};
Params defaults(Kind kind);
// Centrada en X/Z con la base en y = 0 (como las piezas de construccion).
PolyMesh make(Kind kind, const Params& params);
// Un poligono (en XZ) extruido hacia arriba `height`.
PolyMesh extrudePolygon(const std::vector<Vec2>& outline, float height, bool flip = false);
}  // namespace shapes

// --- Operaciones ----------------------------------------------------------------------

namespace ops {

// Vertices de unas caras / aristas (sin repetir).
std::vector<std::uint32_t> verticesOfFaces(const PolyMesh& m, const std::vector<int>& faces);
std::vector<std::uint32_t> verticesOfEdges(const std::vector<Edge>& edges);
// Mover / girar / escalar vertices con una matriz (en el espacio de la malla).
void transformVertices(PolyMesh& m, const std::vector<std::uint32_t>& vertices, const Mat4& matrix);

enum class ExtrudeMode : int { Group = 0, Individual = 1 };
// Devuelve las caras extruidas (las de arriba). distance 0 = solo duplica el contorno.
std::vector<int> extrudeFaces(PolyMesh& m, const std::vector<int>& faces, float distance,
                              ExtrudeMode mode = ExtrudeMode::Group);
// Aristas de borde hacia fuera en el plano de su cara (o a lo largo de la normal
// con along_normal). Devuelve las aristas nuevas (las de fuera).
std::vector<Edge> extrudeEdges(PolyMesh& m, const std::vector<Edge>& edges, float distance, bool along_normal = false);
// Cara mas pequena dentro de cada cara (o de la region). Devuelve las de dentro.
std::vector<int> insetFaces(PolyMesh& m, const std::vector<int>& faces, float amount, bool individual = false);
// Chaflan de aristas interiores (cara nueva en lugar de la arista). Devuelve las caras del bisel.
std::vector<int> bevelEdges(PolyMesh& m, const std::vector<Edge>& edges, float amount);
// Cada cara en N quads (centro + puntos medios). Devuelve las caras nuevas.
std::vector<int> subdivideFaces(PolyMesh& m, const std::vector<int>& faces);
// Corta cada arista en `cuts` + 1 trozos. Devuelve los vertices nuevos.
std::vector<std::uint32_t> subdivideEdges(PolyMesh& m, const std::vector<Edge>& edges, int cuts = 1);
// Une los puntos medios de las aristas elegidas dentro de cada cara (corta caras).
std::vector<Edge> connectEdges(PolyMesh& m, const std::vector<Edge>& edges);
// Une los vertices elegidos dentro de cada cara (dos de la misma cara = corte).
std::vector<Edge> connectVertices(PolyMesh& m, const std::vector<std::uint32_t>& vertices);
// Anillo / bucle de aristas a partir de una (por quads).
std::vector<Edge> edgeRing(const PolyMesh& m, const Edge& start);
std::vector<Edge> edgeLoop(const PolyMesh& m, const Edge& start);
// Bucle nuevo cortando el anillo de la arista (t = donde corta, 0..1).
std::vector<Edge> insertEdgeLoop(PolyMesh& m, const Edge& start, float t = 0.5f);
// Cara entre dos aristas de borde. -1 si no se puede.
int bridgeEdges(PolyMesh& m, const Edge& a, const Edge& b);
// Tapa los agujeros (bucles de borde) que tocan esas aristas (vacio = todos). Devuelve las caras nuevas.
std::vector<int> fillHoles(PolyMesh& m, const std::vector<Edge>& edges = {});
// Une caras vecinas en una (si su contorno es un solo bucle). -1 si no se puede.
int mergeFaces(PolyMesh& m, const std::vector<int>& faces);
void deleteFaces(PolyMesh& m, const std::vector<int>& faces);
void flipFaces(PolyMesh& m, const std::vector<int>& faces);
// Orientacion coherente entre vecinas y hacia fuera (volumen positivo).
void conformNormals(PolyMesh& m);
std::vector<int> triangulateFaces(PolyMesh& m, const std::vector<int>& faces);
// Suelda los vertices a menos de `distance` (de la lista, o todos si esta vacia). Devuelve cuantos quito.
int weldVertices(PolyMesh& m, const std::vector<std::uint32_t>& vertices, float distance);
// Junta los vertices en su centro. Devuelve el que queda.
std::uint32_t collapseVertices(PolyMesh& m, const std::vector<std::uint32_t>& vertices);
// Cada cara con su propia copia de esos vertices.
void splitVertices(PolyMesh& m, const std::vector<std::uint32_t>& vertices);
// Copia de las caras en la misma malla (con vertices nuevos). Devuelve las copias.
std::vector<int> duplicateFaces(PolyMesh& m, const std::vector<int>& faces);
// Las caras a una malla aparte (y se quitan de esta si `remove`).
PolyMesh detachFaces(PolyMesh& m, const std::vector<int>& faces, bool remove = true);
// Mezcla otra malla (con su matriz) dentro de esta.
void append(PolyMesh& m, const PolyMesh& other, const Mat4& transform = Mat4::identity());
// Suavizado de Catmull-Clark (cada nivel: todo quads, 4 veces mas caras).
void subdivideSmooth(PolyMesh& m, int levels = 1);
// Relajar (Laplaciano): acerca los vertices a la media de sus vecinos.
void relax(PolyMesh& m, const std::vector<std::uint32_t>& vertices, float amount = 0.5f, int iterations = 1,
           bool keep_border = true);
// Espejo en un eje (0 X, 1 Y, 2 Z) por el plano `plane`; con `keep` conserva la
// mitad original y suelda la costura (modelar con simetria).
void mirror(PolyMesh& m, int axis, bool keep = true, float plane = 0.0f);
// Centra la malla en su origen (devuelve cuanto se movio: la entidad debe moverse igual).
Vec3 centerPivot(PolyMesh& m, bool bottom = false);
void snapToGrid(PolyMesh& m, const std::vector<std::uint32_t>& vertices, float grid);
// Ruido en la direccion de la normal (rocas, terreno organico).
void randomize(PolyMesh& m, const std::vector<std::uint32_t>& vertices, float amount, std::uint32_t seed = 1);

// --- Seleccion ---
std::vector<int> growFaces(const PolyMesh& m, const std::vector<int>& faces);
std::vector<int> shrinkFaces(const PolyMesh& m, const std::vector<int>& faces);
// Caras conectadas (por aristas) a esas.
std::vector<int> linkedFaces(const PolyMesh& m, const std::vector<int>& faces);
// Caras vecinas con normal parecida (planas como la primera, en grados).
std::vector<int> facesByAngle(const PolyMesh& m, int seed, float max_angle = 10.0f);
std::vector<int> facesWithMaterial(const PolyMesh& m, int material);

// --- Booleanas (BSP) ---
enum class BoolOp : int { Union = 0, Subtract = 1, Intersect = 2 };
// a y b con sus matrices de mundo; el resultado en el espacio de a.
PolyMesh boolean(const PolyMesh& a, const Mat4& a_world, const PolyMesh& b, const Mat4& b_world, BoolOp op);

// De triangulos (un modelo importado, una malla creada por codigo) a malla
// editable: suelda las posiciones iguales y junta en quads las parejas de
// triangulos casi coplanares (menos de `quad_angle` grados) con el mismo
// material. `materials`: uno por triangulo (vacio = 0).
PolyMesh fromTriangles(const std::vector<Vec3>& positions, const std::vector<std::uint32_t>& indices,
                       const std::vector<int>& materials = {}, float quad_angle = 2.0f);

// Wavefront OBJ (con UV y normales planas).
std::string toObj(const PolyMesh& m, const std::string& name = "malla");

}  // namespace ops

}  // namespace cramion::modeling

#endif  // CRAMION_CORE_MODELING_EDITABLE_MESH_H
