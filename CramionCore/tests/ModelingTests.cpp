// Pruebas del modelado poligonal (consola, sin GPU): formas, operaciones de
// topologia, booleanas, UV, guardar/leer y el componente EditableMesh.
// Devuelve 0 si todo va.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/RuntimeMesh.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/modeling/EditableMesh.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>

using namespace cramion;
using namespace cramion::modeling;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what.c_str());
    if (!condition) ++failures;
}

double volume(const PolyMesh& m) {
    double v = 0.0;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        const std::vector<std::uint32_t> tris = m.triangulateFace(static_cast<int>(fi));
        for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
            v += static_cast<double>(core::dot(m.positions[f.v[tris[t]]],
                                               core::cross(m.positions[f.v[tris[t + 1]]], m.positions[f.v[tris[t + 2]]])));
        }
    }
    return v / 6.0;
}

// Cerrada y bien orientada: cada arista dirigida una vez y su inversa tambien.
bool watertight(const PolyMesh& m) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> count;
    for (const Face& f : m.faces) {
        for (std::size_t i = 0; i < f.v.size(); ++i) ++count[{f.v[i], f.v[(i + 1) % f.v.size()]}];
    }
    for (const auto& [e, n] : count) {
        if (n != 1) return false;
        const auto back = count.find({e.second, e.first});
        if (back == count.end() || back->second != 1) return false;
    }
    return !m.faces.empty();
}

PolyMesh unitCube() {
    shapes::Params p = shapes::defaults(shapes::Kind::Cube);
    return shapes::make(shapes::Kind::Cube, p);
}

int topFace(const PolyMesh& m) {
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        if (m.faceNormal(static_cast<int>(fi)).y > 0.99f) return static_cast<int>(fi);
    }
    return -1;
}

Edge edgeAt(const PolyMesh& m, const Vec3& a, const Vec3& b) {
    std::uint32_t ia = 0, ib = 0;
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        if (core::length(m.positions[i] - a) < 1e-4f) ia = static_cast<std::uint32_t>(i);
        if (core::length(m.positions[i] - b) < 1e-4f) ib = static_cast<std::uint32_t>(i);
    }
    return Edge(ia, ib);
}

void testShapes() {
    std::printf("Formas\n");
    for (int k = 0; k < static_cast<int>(shapes::Kind::Count); ++k) {
        const auto kind = static_cast<shapes::Kind>(k);
        const PolyMesh m = shapes::make(kind, shapes::defaults(kind));
        const std::string name = shapes::kindName(kind);
        check(m.validate().empty() && !m.faces.empty(), name + ": malla valida");
        shapes::Kind back{};
        check(shapes::kindFromKey(shapes::kindKey(kind), back) && back == kind, name + ": clave " + shapes::kindKey(kind));
        const bool open = kind == shapes::Kind::Plane;
        if (!open) {
            check(watertight(m), name + ": cerrada y bien orientada");
            const double v = volume(m);
            check(kind == shapes::Kind::Room ? v < 0.0 : v > 0.0, name + ": normales hacia " +
                                                                  (kind == shapes::Kind::Room ? "dentro" : "fuera"));
        }
        Vec3 lo{}, hi{};
        m.bounds(lo, hi);
        check(std::abs(lo.y) < 1e-3f, name + ": la base en y = 0");
    }
    check(std::abs(volume(unitCube()) - 1.0) < 1e-4, "cubo: volumen 1");
    shapes::Params cyl = shapes::defaults(shapes::Kind::Cylinder);
    cyl.segments = 64;
    const double cv = volume(shapes::make(shapes::Kind::Cylinder, cyl));
    check(std::abs(cv - core::kPi * 0.25 * 2.0) < 0.02, "cilindro: volumen pi r2 h");
    shapes::Params sph = shapes::defaults(shapes::Kind::Sphere);
    sph.segments = 48;
    sph.rings = 24;
    const double sv = volume(shapes::make(shapes::Kind::Sphere, sph));
    check(std::abs(sv - 4.0 / 3.0 * core::kPi * 0.125) < 0.02, "esfera: volumen 4/3 pi r3");
    shapes::Params sub;
    sub.subdivisions_x = 3;
    sub.subdivisions_y = 2;
    sub.subdivisions_z = 4;
    const PolyMesh grid_cube = shapes::make(shapes::Kind::Cube, sub);
    check(grid_cube.faces.size() == 2 * (3 * 4 + 3 * 2 + 4 * 2) && watertight(grid_cube), "cubo subdividido: caras y cerrado");
    shapes::Params stairs = shapes::defaults(shapes::Kind::Stairs);
    stairs.sides = false;
    const PolyMesh slab = shapes::make(shapes::Kind::Stairs, stairs);
    check(watertight(slab) && volume(slab) > 0.0, "escalera de losa: cerrada");
    const PolyMesh extruded = shapes::extrudePolygon({{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}}, 1.0f);
    check(watertight(extruded) && std::abs(volume(extruded) - 3.0) < 1e-4, "poligono en L extruido: volumen 3");
}

void testExtrudeInsetBevel() {
    std::printf("Extruir, inset y bisel\n");
    {
        PolyMesh m = unitCube();
        const int top = topFace(m);
        const std::vector<int> result = ops::extrudeFaces(m, {top}, 0.5f);
        check(m.faces.size() == 10 && watertight(m), "extruir la cara de arriba: 10 caras, cerrada");
        check(std::abs(volume(m) - 1.5) < 1e-4, "volumen + 0.5");
        check(result.size() == 1 && std::abs(m.faceCenter(result[0]).y - 1.5f) < 1e-4f, "devuelve la cara extruida (arriba)");
    }
    {
        PolyMesh m = unitCube();
        std::vector<int> all;
        for (std::size_t i = 0; i < m.faces.size(); ++i) all.push_back(static_cast<int>(i));
        ops::extrudeFaces(m, all, 0.1f, ops::ExtrudeMode::Individual);
        check(m.faces.size() == 30 && m.validate().empty(), "extruir cada cara por separado: 6 + 24 caras");
    }
    {
        PolyMesh m;
        shapes::Params p;
        p.size = Vec3{2.0f, 1.0f, 1.0f};
        p.subdivisions_x = 2;
        m = shapes::make(shapes::Kind::Cube, p);
        std::vector<int> tops;
        for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
            if (m.faceNormal(static_cast<int>(fi)).y > 0.99f) tops.push_back(static_cast<int>(fi));
        }
        ops::extrudeFaces(m, tops, 1.0f);
        check(tops.size() == 2 && watertight(m) && std::abs(volume(m) - 4.0) < 1e-3, "extruir dos caras juntas (grupo): sin pared entre ellas");
    }
    {
        PolyMesh m = unitCube();
        const int top = topFace(m);
        ops::insetFaces(m, {top}, 0.2f);
        check(m.faces.size() == 10 && watertight(m) && std::abs(volume(m) - 1.0) < 1e-4, "inset: 10 caras, misma forma");
        check(std::abs(m.faceArea(top) - 0.36f) < 1e-3f, "inset 0.2: la cara de dentro mide 0.6 x 0.6");
        ops::extrudeFaces(m, {top}, -0.3f);
        check(watertight(m) && std::abs(volume(m) - (1.0 - 0.36 * 0.3)) < 1e-3, "inset + extruir hacia dentro: un hueco");
    }
    {
        PolyMesh m = unitCube();
        const Edge e = edgeAt(m, Vec3{-0.5f, 1.0f, 0.5f}, Vec3{0.5f, 1.0f, 0.5f});
        const std::vector<int> bevel = ops::bevelEdges(m, {e}, 0.2f);
        check(bevel.size() == 1 && m.faces.size() == 7 && watertight(m), "bisel de una arista: 7 caras, cerrada");
        check(std::abs(volume(m) - (1.0 - 0.02)) < 1e-3, "el bisel quita el prisma de la esquina");
    }
    {
        PolyMesh m = unitCube();
        const Vec3 corner{0.5f, 1.0f, 0.5f};
        const std::vector<Edge> edges = {edgeAt(m, corner, Vec3{-0.5f, 1.0f, 0.5f}), edgeAt(m, corner, Vec3{0.5f, 1.0f, -0.5f}),
                                         edgeAt(m, corner, Vec3{0.5f, 0.0f, 0.5f})};
        ops::bevelEdges(m, edges, 0.1f);
        check(watertight(m), "bisel de las tres aristas de una esquina: tapa el triangulo de la esquina");
    }
    {
        PolyMesh m = unitCube();
        ops::bevelEdges(m, m.edges(), 0.1f);
        check(watertight(m) && m.faces.size() == 6 + 12 + 8, "bisel de todas: 26 caras (6 + 12 + 8 esquinas)");
    }
    {
        PolyMesh m = shapes::make(shapes::Kind::Plane, shapes::defaults(shapes::Kind::Plane));
        const std::vector<Edge> border = m.borderEdges();
        ops::extrudeEdges(m, border, 1.0f);
        Vec3 lo{}, hi{};
        m.bounds(lo, hi);
        check(std::abs(hi.x - 3.0f) < 1e-3f && m.borderEdges().size() == border.size(), "extruir el borde de un plano: crece 1 m");
    }
}

void testSubdivideConnectLoops() {
    std::printf("Subdividir, conectar y bucles\n");
    {
        PolyMesh m = unitCube();
        ops::subdivideFaces(m, {topFace(m)});
        check(m.faces.size() == 9 && watertight(m) && std::abs(volume(m) - 1.0) < 1e-4, "subdividir la de arriba: 4 quads, cerrada");
    }
    {
        PolyMesh m = unitCube();
        const Edge a = edgeAt(m, Vec3{-0.5f, 1.0f, 0.5f}, Vec3{0.5f, 1.0f, 0.5f});
        const Edge b = edgeAt(m, Vec3{-0.5f, 1.0f, -0.5f}, Vec3{0.5f, 1.0f, -0.5f});
        const std::vector<Edge> cut = ops::connectEdges(m, {a, b});
        check(cut.size() == 1 && m.faces.size() == 7 && watertight(m), "conectar dos aristas opuestas: parte la cara");
    }
    {
        PolyMesh m = unitCube();
        const Edge e = edgeAt(m, Vec3{-0.5f, 1.0f, 0.5f}, Vec3{0.5f, 1.0f, 0.5f});
        check(ops::edgeRing(m, e).size() == 4, "anillo de aristas alrededor del cubo: 4");
        const std::vector<Edge> loop = ops::insertEdgeLoop(m, e, 0.25f);
        check(loop.size() == 4 && m.faces.size() == 10 && watertight(m), "insertar bucle: 4 aristas nuevas, cerrada");
        int at_quarter = 0;
        for (const Vec3& p : m.positions) at_quarter += std::abs(p.x - (-0.25f)) < 1e-4f ? 1 : 0;
        check(at_quarter == 4, "el bucle corta donde se pidio (t = 0.25)");
    }
    {
        shapes::Params p;
        p.size = Vec3{4.0f, 0.0f, 4.0f};
        p.subdivisions_x = p.subdivisions_z = 4;
        const PolyMesh m = shapes::make(shapes::Kind::Plane, p);
        const Edge e = edgeAt(m, Vec3{-1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f});
        check(ops::edgeLoop(m, e).size() == 4, "bucle de aristas por una rejilla: de borde a borde");
    }
    {
        PolyMesh m = unitCube();
        const Edge e = edgeAt(m, Vec3{-0.5f, 1.0f, 0.5f}, Vec3{0.5f, 1.0f, 0.5f});
        ops::subdivideEdges(m, {e}, 3);
        check(m.positions.size() == 11 && watertight(m), "cortar una arista en 4: las dos caras la reciben");
    }
    {
        PolyMesh m = unitCube();
        const int top = topFace(m);
        const Face f = m.faces[static_cast<std::size_t>(top)];
        ops::connectVertices(m, {f.v[0], f.v[2]});
        check(m.faces.size() == 7 && watertight(m), "conectar dos vertices opuestos: diagonal");
    }
}

void testTopology() {
    std::printf("Rellenar, unir, soldar, puentes\n");
    {
        PolyMesh m = unitCube();
        ops::deleteFaces(m, {topFace(m)});
        check(m.borderEdges().size() == 4, "quitar la tapa: 4 aristas de borde");
        const std::vector<int> filled = ops::fillHoles(m);
        check(filled.size() == 1 && watertight(m) && std::abs(volume(m) - 1.0) < 1e-4, "rellenar el agujero");
    }
    {
        PolyMesh m = unitCube();
        const std::vector<int> quads = ops::subdivideFaces(m, {topFace(m)});
        const int merged = ops::mergeFaces(m, quads);
        check(merged >= 0 && m.faces.size() == 6 && watertight(m), "unir los 4 quads en una cara");
    }
    {
        PolyMesh m = unitCube();
        PolyMesh other = unitCube();
        ops::append(m, other, core::translate(Vec3{1.0f, 0.0f, 0.0f}));
        const int welded = ops::weldVertices(m, {}, 1e-4f);
        check(welded == 4 && m.positions.size() == 12, "soldar dos cubos pegados: 4 vertices comunes");
    }
    {
        PolyMesh m;
        m.positions = {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 2, 0}, {1, 2, 0}, {1, 2, 1}, {0, 2, 1}};
        m.addFace({0, 1, 2, 3});
        m.addFace({7, 6, 5, 4});
        const int bridge = ops::bridgeEdges(m, Edge(0, 1), Edge(4, 5));
        check(bridge >= 0 && m.faces[static_cast<std::size_t>(bridge)].v.size() == 4, "puente entre dos aristas");
        ops::conformNormals(m);
        check(m.validate().empty(), "valida tras el puente");
    }
    {
        PolyMesh m = unitCube();
        ops::flipFaces(m, {0, 2, 3});
        ops::conformNormals(m);
        check(watertight(m) && volume(m) > 0.0, "conformar normales: todas hacia fuera");
    }
    {
        PolyMesh m;
        m.positions = {{0, 0, 0}, {2, 0, 0}, {2, 0, -1}, {1, 0, -1}, {1, 0, -2}, {0, 0, -2}};
        m.addFace({0, 1, 2, 3, 4, 5});
        const std::vector<std::uint32_t> tris = m.triangulateFace(0);
        double area = 0.0;
        for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
            const Vec3 a = m.positions[tris[t]], b = m.positions[tris[t + 1]], c = m.positions[tris[t + 2]];
            const Vec3 n = core::cross(b - a, c - a);
            area += 0.5 * n.y;
        }
        check(tris.size() == 12 && std::abs(area - 3.0) < 1e-4, "triangular una L concava: 4 triangulos, area 3, bien orientados");
    }
    {
        PolyMesh m = unitCube();
        ops::splitVertices(m, {0});
        check(m.positions.size() == 10, "separar un vertice de 3 caras: 2 copias nuevas");
        ops::weldVertices(m, {}, 1e-4f);
        check(m.positions.size() == 8 && watertight(m), "y soldarlo de vuelta");
    }
    {
        PolyMesh m = unitCube();
        const int top = topFace(m);
        PolyMesh piece = ops::detachFaces(m, {top});
        check(piece.faces.size() == 1 && piece.positions.size() == 4 && m.faces.size() == 5, "separar una cara a otra malla");
    }
    {
        PolyMesh m = unitCube();
        const std::vector<int> grown = ops::growFaces(m, {topFace(m)});
        check(grown.size() == 5, "crecer la seleccion: la de arriba y sus 4 vecinas");
        check(ops::shrinkFaces(m, grown).size() == 1, "encoger: queda la de dentro");
        check(ops::linkedFaces(m, {0}).size() == 6, "conectadas: todo el cubo");
        shapes::Params p;
        p.size = Vec3{4.0f, 0.0f, 4.0f};
        p.subdivisions_x = p.subdivisions_z = 3;
        const PolyMesh plane = shapes::make(shapes::Kind::Plane, p);
        check(ops::facesByAngle(plane, 0, 5.0f).size() == 9, "por angulo: todo el plano");
    }
}

void testSmoothMirrorBoolean() {
    std::printf("Catmull-Clark, espejo, booleanas\n");
    {
        PolyMesh m = unitCube();
        ops::subdivideSmooth(m, 1);
        const double v = volume(m);
        check(m.faces.size() == 24 && watertight(m) && std::abs(v - 7.0 / 16.0) < 1e-4,
              "Catmull-Clark: 24 quads, cerrada, volumen 7/16 (mas redonda)");
        ops::subdivideSmooth(m, 1);
        check(m.faces.size() == 96 && watertight(m), "segundo nivel: 96");
    }
    {
        PolyMesh m = unitCube();
        for (Vec3& p : m.positions) p.x += 0.5f;  // de x = 0 a 1
        ops::mirror(m, 0, true, 0.0f);
        Vec3 lo{}, hi{};
        m.bounds(lo, hi);
        check(std::abs(lo.x + 1.0f) < 1e-4f && watertight(m) && std::abs(volume(m) - 2.0) < 1e-3,
              "espejo con costura: dos mitades soldadas, sin caras dentro");
    }
    {
        PolyMesh m = unitCube();
        const Vec3 offset = ops::centerPivot(m, false);
        check(std::abs(offset.y - 0.5f) < 1e-4f, "centrar pivote: devuelve el desplazamiento");
    }
    const PolyMesh cube = unitCube();
    const Mat4 at_origin = Mat4::identity();
    {
        const PolyMesh r = ops::boolean(cube, at_origin, cube, core::translate(Vec3{0.5f, 0.5f, 0.5f}), ops::BoolOp::Subtract);
        check(r.validate().empty() && std::abs(volume(r) - (1.0 - 0.125)) < 1e-3, "resta: cubo menos la esquina (0.875)");
    }
    {
        const PolyMesh r = ops::boolean(cube, at_origin, cube, core::translate(Vec3{0.5f, 0.0f, 0.0f}), ops::BoolOp::Union);
        check(std::abs(volume(r) - 1.5) < 1e-3, "union de dos cubos solapados (1.5)");
    }
    {
        const PolyMesh r = ops::boolean(cube, at_origin, cube, core::translate(Vec3{0.5f, 0.0f, 0.0f}), ops::BoolOp::Intersect);
        check(std::abs(volume(r) - 0.5) < 1e-3, "interseccion (0.5)");
    }
    {
        shapes::Params p = shapes::defaults(shapes::Kind::Cylinder);
        p.size = Vec3{0.5f, 3.0f, 0.5f};
        const PolyMesh hole = shapes::make(shapes::Kind::Cylinder, p);
        const PolyMesh r = ops::boolean(cube, at_origin, hole, core::translate(Vec3{0.0f, -1.0f, 0.0f}), ops::BoolOp::Subtract);
        check(volume(r) < 1.0 - 0.15 && volume(r) > 1.0 - 0.25, "agujero de cilindro atravesando el cubo");
    }
}

void testUvAndBuild() {
    std::printf("UV, malla del renderizador y guardar\n");
    PolyMesh wall;
    wall.positions = {{0, 0, 0}, {2, 0, 0}, {2, 3, 0}, {0, 3, 0}};
    wall.addFace({0, 1, 2, 3});
    const Vec2 a = faceCornerUv(wall, 0, 0, Mat4::identity());
    const Vec2 b = faceCornerUv(wall, 0, 1, Mat4::identity());
    const Vec2 c = faceCornerUv(wall, 0, 2, Mat4::identity());
    check(std::abs((b.x - a.x) - 2.0f) < 1e-4f && std::abs((a.y - c.y) - 3.0f) < 1e-4f, "UV caja: metros (2 x 3), v hacia arriba");
    wall.faces[0].uvs.fill = UvFill::Stretch;
    const Vec2 s0 = faceCornerUv(wall, 0, 0, Mat4::identity());
    const Vec2 s2 = faceCornerUv(wall, 0, 2, Mat4::identity());
    check(std::abs(s0.x) < 1e-4f && std::abs(s0.y - 1.0f) < 1e-4f && std::abs(s2.x - 1.0f) < 1e-4f && std::abs(s2.y) < 1e-4f,
          "UV estirada: de 0 a 1");
    wall.faces[0].uvs.fill = UvFill::Tile;
    wall.faces[0].uvs.scale = Vec2{2.0f, 1.0f};
    wall.faces[0].uvs.offset = Vec2{0.5f, 0.0f};
    const Vec2 t1 = faceCornerUv(wall, 0, 1, Mat4::identity());
    check(std::abs(t1.x - 1.5f) < 1e-4f, "escala 2 m por repeticion y desplazamiento");
    wall.faces[0].uvs.world_space = true;
    const Vec2 w0 = faceCornerUv(wall, 0, 0, core::translate(Vec3{4.0f, 0.0f, 0.0f}));
    check(std::abs(w0.x - 2.5f) < 1e-4f, "UV en el mundo: depende de la posicion del objeto");

    PolyMesh m = unitCube();
    m.faces[0].material = 2;
    m.faces[1].smoothing = 1;
    m.faces[2].uvs.mode = UvMode::Manual;
    m.faces[2].uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    m.faces[3].uvs.rotation = 45.0f;
    m.faces[3].uvs.flip_v = true;
    const std::string text = m.serialize();
    PolyMesh back;
    check(back.deserialize(text) && back.serialize() == text, "guardar y leer la malla (texto)");
    check(back.faces[0].material == 2 && back.faces[2].uv.size() == 4 && back.faces[3].uvs.flip_v, "conserva material, UV manuales y ajustes");
    check(!back.deserialize("PM1 3 1 0 0 0 1 0 0"), "texto roto: no se lee (y no cambia la malla)");

    BuildOptions options;
    options.slots = {SlotMaterial{}, SlotMaterial{}, SlotMaterial{Vec3{1, 0, 0}}};
    const std::shared_ptr<ecs::Mesh> built = buildMesh(m, options);
    check(built->validate().empty() && built->triangleCount() == 12, "malla del renderizador: 12 triangulos");
    check(built->subMeshCount() == 3 && built->triangles(2).size() == 6, "un hueco de material por submalla");
    check(built->vertices.size() == 24 && built->uv.size() == 24, "un vertice por esquina (24) con UV");

    // De vuelta desde triangulos (vertices por esquina, como un modelo importado).
    {
        const std::shared_ptr<ecs::Mesh> cube_mesh = buildMesh(unitCube(), BuildOptions{});
        std::vector<int> tri_materials;
        const std::vector<std::uint32_t> all = cube_mesh->allTriangles();
        const PolyMesh rebuilt = ops::fromTriangles(cube_mesh->vertices, all, tri_materials);
        check(rebuilt.positions.size() == 8 && rebuilt.faces.size() == 6 && watertight(rebuilt),
              "convertir triangulos: suelda (8 vertices) y junta en quads (6 caras)");
        const PolyMesh raw = ops::fromTriangles(cube_mesh->vertices, all, tri_materials, 0.0f);
        check(raw.faces.size() == 12 && watertight(raw), "sin juntar: 12 triangulos");
    }

    // Suavizado automatico: una esfera sin grupos, lisa con 60 grados.
    shapes::Params sp = shapes::defaults(shapes::Kind::Sphere);
    sp.smooth = false;
    PolyMesh sphere = shapes::make(shapes::Kind::Sphere, sp);
    BuildOptions smooth;
    smooth.auto_smooth_angle = 60.0f;
    const std::shared_ptr<ecs::Mesh> smooth_mesh = buildMesh(sphere, smooth);
    const std::shared_ptr<ecs::Mesh> flat_mesh = buildMesh(sphere, BuildOptions{});
    float diff = 0.0f;
    for (std::size_t i = 0; i < smooth_mesh->normals.size(); ++i) diff += core::length(smooth_mesh->normals[i] - flat_mesh->normals[i]);
    check(diff > 1.0f, "suavizado automatico: normales de vertice suaves");
}

void testComponent() {
    std::printf("Componente EditableMesh\n");
    ecs::World world;
    ecs::Entity e = createEditableEntity(world, unitCube(), "Bloque", {});
    check(e.has<ecs::MeshRenderer>() && e.get<ecs::MeshRenderer>().mesh != nullptr, "crea su MeshRenderer con la malla");
    const auto first = e.get<ecs::MeshRenderer>().mesh;
    updateEditableMeshes(world);
    check(e.get<ecs::MeshRenderer>().mesh == first, "sin cambios no se rehace");
    EditableMesh& em = e.get<EditableMesh>();
    ops::extrudeFaces(em.mesh, {topFace(em.mesh)}, 1.0f);
    em.markModified();
    updateEditableMeshes(world);
    check(e.get<ecs::MeshRenderer>().mesh != first && e.get<ecs::MeshRenderer>().mesh->triangleCount() == 20,
          "al cambiar se rehace (20 triangulos)");
    em.slots[0].color = Vec3{0.2f, 0.4f, 0.9f};
    const auto before = e.get<ecs::MeshRenderer>().mesh;
    updateEditableMeshes(world);
    check(e.get<ecs::MeshRenderer>().mesh != before, "cambiar el color de un hueco tambien la rehace");

    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_modeling_test.crscene";
    std::string error;
    check(ecs::saveScene(world, file, &error), "guardar la escena");
    ecs::World loaded;
    check(ecs::loadScene(loaded, file, &error), "leerla");
    int found = 0;
    for (const entt::entity h : loaded.registry().view<EditableMesh>()) {
        const EditableMesh& l = loaded.registry().get<EditableMesh>(h);
        found = static_cast<int>(l.mesh.faces.size());
        check(std::abs(l.slots[0].color.z - 0.9f) < 1e-4f, "conserva los huecos de material");
    }
    check(found == 10, "la malla vuelve con sus 10 caras");
    updateEditableMeshes(loaded);
    bool has_mesh = false;
    for (const entt::entity h : loaded.registry().view<EditableMesh>()) {
        has_mesh = loaded.registry().get<ecs::MeshRenderer>(h).mesh != nullptr;
    }
    check(has_mesh, "tras leerla se genera su malla");
    std::filesystem::remove(file);
    const std::string obj = ops::toObj(unitCube());
    check(obj.find("\nf ") != std::string::npos && obj.find("vt ") != std::string::npos, "exportar OBJ");
}

}  // namespace

int main() {
    testShapes();
    testExtrudeInsetBevel();
    testSubdivideConnectLoops();
    testTopology();
    testSmoothMirrorBoolean();
    testUvAndBuild();
    testComponent();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
